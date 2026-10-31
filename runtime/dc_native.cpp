// Native PC rendering, Dark Cloud's adapter: DC's 3D models read from the game's own data and drawn by the fork's
// native renderer (ps2x::native::Renderer), not by VU1 and the GS.
//
// DC draws every model piece with CVisualVu1::DrawVu1(packet, local-to-world matrix, RenderInfo *, VU1_PROGRAM, ...)
// (0x135000; CVisualMDTVu1::DrawVu1 0x1360E0 and CFrameVu1::DrawVu1 0x129400 end there). It writes a VIF1 header
// into the packet (the object-to-screen matrix at words 0x20-0x2F, lights, the GS PRMODE) and references the
// visual's VU data, which CVisualVu1::CreateVUdataFromMDT (0x135AA0) built once from the model (MDT): per material a
// DIRECT with TEX0 (and TEX1), an UNPACK of the material to VU 0x25, then per strip an UNPACK (TOPS-relative) of a GIF
// tag, a (count, type, colour flag) quadword and the strip's positions, normals, texture coordinates and colours, one
// array each (SetVuData 0x135970), closed by MSCAL / MSCNT. The microprogram transforms each position by the matrix,
// divides by w and converts with FTOI4: the renderer's draw() does the same with gsZScale 16.
//
// DC_NATIVE3D=1 turns it on. First step: geometry and textures, unlit; VU1 still draws (PS2X_NATIVE_VIEW=only shows
// the native frame alone).

#include "ps2_runtime.h"
#include "runtime/native/native_renderer.h"
#include "runtime/native/native_vif1.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace dc::native
{
    namespace
    {
        constexpr uint32_t kVisualDrawVu1 = 0x00135000u; // CVisualVu1::DrawVu1(uint *, float (*)[4], RenderInfo *, ...)
        PS2Runtime::RecompiledFunction g_drawVu1 = nullptr;
        uint64_t g_group = 0;

        struct Stats
        {
            uint64_t calls = 0, empty = 0, noData = 0, visuals = 0, strips = 0, triangles = 0, skipped = 0;
            uint64_t shadows = 0, screenSpace = 0; // DrawVu1 calls left to VU1: RenderInfo + 0x320 set, program 0xF
        } g_stats;

        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        float rdf(const uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        uint64_t rd64(const uint8_t *rdram, uint32_t address)
        {
            uint64_t value = 0;
            std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        ps2x::native::Screen screen()
        {
            // DC's drawing environment: 640 x 224 fields, XYOFFSET (1728, 1936) (0x6C00, 0x7900 in 12.4).
            ps2x::native::Screen s;
            s.originX = 1728.0f;
            s.originY = 1936.0f;
            s.width = 640.0f;
            s.height = 224.0f;
            s.depthMax = 16777215.0f;
            s.gsZScale = 16.0f;
            return s;
        }

        // Which VU1 programs this adapter draws, told from the VIF1 stream itself: DC starts nearly every microprogram
        // at pc 0 with the same code image, so the code cannot tell a model from the rest. The observer reads each piece
        // of a transfer on the VIF1 thread (up to each MSCAL / MSCNT, before VIF1 asks the gate) and notes whether it
        // holds a model strip (an UNPACK of SetVuData's layout: a GIF tag whose upper word is 0x302E4000 (strip) or
        // 0x302DC000 (list)), and the PRMODE of the latest DrawVu1 header (IIP clear: a shadow volume; FST: the
        // screen-space program 0xF), which stay on VU1. The gate then skips exactly the MSCALs the hook drew, in the
        // same order, so the renderer groups (1, 2, ...) match the VIF1 interpreter's numbering of skipped programs.
        std::atomic<uint64_t> g_gateCalls{0}, g_gateSkipped{0}, g_pieces{0}, g_modelUnpacks{0}, g_unpacks6c{0},
            g_gateWithModel{0};
        std::atomic<uint32_t> g_lastTagHigh{0}, g_lastPrmode{0};
        bool g_pieceHasModel = false; // strips of a model since the last MSCAL / MSCNT
        uint64_t g_prmode = 0x48u;    // IIP + ABE, as DrawVu1 writes for a lit model
        // One decision per MSCAL of the observed stream, in order (the observer sees each whole transfer before VIF1
        // runs it; VIF1 asks the gate at each MSCAL in the same order).
        std::mutex g_decisionsMutex;
        std::deque<bool> g_decisions;

        bool modelPrmode(uint64_t prmode)
        {
            return (prmode & 0x8u) != 0u && (prmode & 0x100u) == 0u;
        }

        void observe(const uint8_t *stream, size_t bytes)
        {
            g_pieces.fetch_add(1u, std::memory_order_relaxed);
            const size_t words = bytes / 4u;
            auto word = [&](size_t i) {
                uint32_t value = 0;
                std::memcpy(&value, stream + i * 4u, sizeof(value));
                return value;
            };
            size_t i = 0;
            while (i < words)
            {
                const uint32_t code = word(i);
                const uint32_t cmd = (code >> 24) & 0x7Fu;
                if (cmd == 0x50u || cmd == 0x51u) // DIRECT / DIRECTHL: a GIF packet
                {
                    const uint32_t qwords = (code & 0xFFFFu) ? (code & 0xFFFFu) : 0x10000u;
                    // DrawVu1's header: an A+D GIF tag (PACKED, NREG 1, REGS 0xE) then (value, register) pairs;
                    // PRMODE is register 0x1B.
                    if (i + 4u < words)
                    {
                        const uint32_t tagLo = word(i + 1u), tagHi = word(i + 2u), regs = word(i + 3u);
                        const uint32_t nloop = tagLo & 0x7FFFu, flg = (tagHi >> 26) & 3u, nreg = (tagHi >> 28) & 15u;
                        if (flg == 0u && nreg == 1u && (regs & 0xFu) == 0xEu)
                            for (uint32_t k = 1; k <= nloop && k < qwords && i + 1u + k * 4u + 3u < words; ++k)
                            {
                                const size_t at = i + 1u + k * 4u;
                                if ((word(at + 2u) & 0xFFu) == 0x1Bu)
                                    g_prmode = word(at);
                            }
                    }
                    i += 1u + static_cast<size_t>(qwords) * 4u;
                }
                else if ((cmd & 0x60u) == 0x60u) // UNPACK
                {
                    const uint32_t num = ((code >> 16) & 0xFFu) ? ((code >> 16) & 0xFFu) : 256u;
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u;
                    const uint32_t bits = num * (vn + 1u) * (32u >> vl);
                    if (cmd == 0x6Cu && num >= 2u && i + 8u < words)
                    {
                        const uint32_t tagHigh = word(i + 2u);
                        g_unpacks6c.fetch_add(1u, std::memory_order_relaxed);
                        if ((code & 0x8000u) != 0u)
                            g_lastTagHigh.store(tagHigh, std::memory_order_relaxed);
                        if (tagHigh == 0x302E4000u || tagHigh == 0x302DC000u)
                        {
                            g_pieceHasModel = true;
                            g_modelUnpacks.fetch_add(1u, std::memory_order_relaxed);
                        }
                    }
                    i += 1u + (bits + 31u) / 32u;
                }
                else if (cmd == 0x4Au) // MPG
                {
                    const uint32_t num = ((code >> 16) & 0xFFu) ? ((code >> 16) & 0xFFu) : 256u;
                    i += 1u + num * 2u;
                }
                else if (cmd == 0x14u || cmd == 0x15u) // MSCAL / MSCALF: VIF1 will ask the gate about this one
                {
                    const bool drawn = g_pieceHasModel && modelPrmode(g_prmode);
                    {
                        std::lock_guard<std::mutex> lock(g_decisionsMutex);
                        if (g_decisions.size() > 65536u)
                            g_decisions.clear();
                        g_decisions.push_back(drawn);
                    }
                    g_pieceHasModel = false;
                    i += 1u;
                }
                else if (cmd == 0x17u) // MSCNT: continues the run the last MSCAL started
                {
                    g_pieceHasModel = false;
                    i += 1u;
                }
                else if (cmd == 0x20u) // STMASK
                    i += 2u;
                else if (cmd == 0x30u || cmd == 0x31u) // STROW / STCOL
                    i += 5u;
                else
                    i += 1u;
            }
        }

        bool gate(const uint8_t *code, uint32_t pc)
        {
            (void)code;
            (void)pc;
            g_gateCalls.fetch_add(1u, std::memory_order_relaxed);
            g_lastPrmode.store(static_cast<uint32_t>(g_prmode), std::memory_order_relaxed);
            bool drawn = false;
            {
                std::lock_guard<std::mutex> lock(g_decisionsMutex);
                if (!g_decisions.empty())
                {
                    drawn = g_decisions.front();
                    g_decisions.pop_front();
                    g_gateWithModel.fetch_add(1u, std::memory_order_relaxed);
                }
            }
            if (drawn)
                g_gateSkipped.fetch_add(1u, std::memory_order_relaxed);
            return drawn;
        }

        // The microprogram's lighting (its setup at pc 0x50, image 8c7dbae50f1ab651): the header's VU 9-11 (RenderInfo
        // + 0x110, four light directions in their columns) times VU 5-7 (the local-to-world matrix's rows, normalised)
        // give the lights in the model's space (vf2-vf4); per vertex, d = max(0, vf2 n.x + vf3 n.y + vf4 n.z), one
        // component per light, and the colour is vf9 (VU 17, ambient) + vf5-vf8 (VU 13-16, the lights' colours) . d,
        // kept to 254 (MINI by vf14.y), then FTOI0. The model's main paths first multiply vf5-vf9 by the material's first
        // quadword (VU 37, f256fcac190325a3 at 0xAD0 / 0xBF8). The texture coordinates are scaled by VU 18's x (vf30).
        struct Lighting
        {
            bool lit = false;
            float dir[3][4]{};   // vf2-vf4
            float colour[4][4]{}; // vf5-vf8
            float ambient[4]{};   // vf9
            float uvScale = 1.0f; // vf30.x
            float material[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // VU 37 (the material's first quadword): scales vf5-vf9
        };

        Lighting lightingFrom(const uint8_t *rdram, uint32_t packet)
        {
            auto vu = [&](uint32_t address, int component) {
                // The header unpacks at VU 0 from the packet's word 0x1C.
                return rdf(rdram, packet + 0x1Cu * 4u + address * 16u + static_cast<uint32_t>(component) * 4u);
            };
            Lighting l;
            l.lit = true;
            for (int j = 0; j < 3; ++j)
                for (int c = 0; c < 4; ++c)
                    l.dir[j][c] = vu(9, c) * vu(5u + j, 0) + vu(10, c) * vu(5u + j, 1) + vu(11, c) * vu(5u + j, 2);
            for (int k = 0; k < 4; ++k)
                for (int c = 0; c < 4; ++c)
                    l.colour[k][c] = vu(13u + k, c);
            for (int c = 0; c < 4; ++c)
                l.ambient[c] = vu(17, c);
            l.uvScale = vu(18, 0);
            if (!(l.uvScale > 0.0f && l.uvScale <= 16.0f))
                l.uvScale = 1.0f;
            return l;
        }

        uint32_t shade(const Lighting &l, float nx, float ny, float nz)
        {
            float d[4];
            for (int c = 0; c < 4; ++c)
                d[c] = std::max(0.0f, l.dir[0][c] * nx + l.dir[1][c] * ny + l.dir[2][c] * nz);
            uint32_t rgba = 0;
            for (int c = 0; c < 4; ++c)
            {
                float v = (l.ambient[c] + l.colour[0][c] * d[0] + l.colour[1][c] * d[1] + l.colour[2][c] * d[2] +
                           l.colour[3][c] * d[3]) *
                          l.material[c];
                v = std::min(v, 254.0f);
                const uint32_t i = v > 0.0f ? static_cast<uint32_t>(v) : 0u;
                rgba |= std::min(i, 255u) << (8 * c);
            }
            return rgba;
        }

        // One strip's vertices as triangles (type 4: a strip, 3: a list).
        void emitStrip(const uint8_t *rdram, uint32_t base, uint32_t count, uint32_t type, bool colours,
                       const Lighting &lighting, std::vector<ps2x::native::Vertex> &out)
        {
            const uint32_t positions = base, normals = base + count * 16u, uvs = base + count * 32u;
            const uint32_t colourArray = base + count * 48u;
            auto vertex = [&](uint32_t i)
            {
                ps2x::native::Vertex v;
                v.x = rdf(rdram, positions + i * 16u);
                v.y = rdf(rdram, positions + i * 16u + 4u);
                v.z = rdf(rdram, positions + i * 16u + 8u);
                v.u = rdf(rdram, uvs + i * 16u) * lighting.uvScale;
                v.v = rdf(rdram, uvs + i * 16u + 4u) * lighting.uvScale;
                v.rgba = lighting.lit ? shade(lighting, rdf(rdram, normals + i * 16u), rdf(rdram, normals + i * 16u + 4u),
                                              rdf(rdram, normals + i * 16u + 8u))
                                      : 0x80808080u;
                if (colours)
                {
                    // The colour array's encoding is not settled yet: the first step keeps the vertices grey.
                    (void)colourArray;
                }
                return v;
            };
            if (type == 4u)
            {
                for (uint32_t i = 2; i < count; ++i)
                {
                    ps2x::native::Vertex a = vertex(i - 2), b = vertex(i - 1), c = vertex(i);
                    if (i & 1u)
                        std::swap(a, b);
                    out.push_back(a);
                    out.push_back(b);
                    out.push_back(c);
                }
            }
            else
            {
                for (uint32_t i = 0; i + 2 < count; i += 3)
                {
                    out.push_back(vertex(i));
                    out.push_back(vertex(i + 1));
                    out.push_back(vertex(i + 2));
                }
            }
        }

        // The visual's VU data (CreateVUdataFromMDT's layout) drawn with the packet's object-to-screen matrix.
        void drawVisual(uint8_t *rdram, uint32_t visual, uint32_t packet, bool drawn, bool lit)
        {
            const uint32_t data = rd32(rdram, visual + 0x18u);
            const uint32_t qwords = rd32(rdram, visual + 0x1Cu);
            if (data == 0u || qwords == 0u || qwords > 0x40000u)
            {
                ++g_stats.noData;
                return;
            }
            // The header the packet unpacks at VU 0 (its word 0x1C on): VU 24-27 hold RenderInfo's 0x230 x 0x10 x the
            // local-to-world matrix (words 0x7C-0x8B, clip space: the microprogram's CLIP tests are on it) and VU 28-31
            // RenderInfo's 0x270 matrix (words 0x8C-0x9B, clip to GS screen, applied before the divide by w).
            float clip[16], viewport[16], matrix[16];
            for (int i = 0; i < 16; ++i)
            {
                clip[i] = rdf(rdram, packet + 0x7Cu * 4u + static_cast<uint32_t>(i) * 4u);
                viewport[i] = rdf(rdram, packet + 0x8Cu * 4u + static_cast<uint32_t>(i) * 4u);
            }
            for (int c = 0; c < 4; ++c) // column c of viewport x clip
                for (int r = 0; r < 4; ++r)
                    matrix[c * 4 + r] = viewport[0 * 4 + r] * clip[c * 4 + 0] + viewport[1 * 4 + r] * clip[c * 4 + 1] +
                                        viewport[2 * 4 + r] * clip[c * 4 + 2] + viewport[3 * 4 + r] * clip[c * 4 + 3];

            Lighting lighting;
            if (lit)
                lighting = lightingFrom(rdram, packet);
            ps2x::native::Material material;
            material.textured = false;
            material.blend = ps2x::native::Material::Blend::Alpha;
            material.depthTest = true;
            material.depthWrite = true;
            std::vector<ps2x::native::Vertex> vertices;
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            // The strips before an MSCAL / MSCNT are what that program draws.
            auto flush = [&]
            {
                if (!vertices.empty())
                {
                    if (drawn)
                    {
                        renderer.draw(material, matrix, vertices.data(), static_cast<uint32_t>(vertices.size()));
                        g_stats.triangles += vertices.size() / 3u;
                    }
                    vertices.clear();
                }
            };

            uint32_t q = 0;
            while (q < qwords)
            {
                const uint32_t at = data + q * 16u;
                const uint32_t code = rd32(rdram, at + 12u);
                const uint32_t cmd = (code >> 24) & 0x7Fu;
                if (cmd == 0x50u) // DIRECT n: a GIF A+D packet (TEX0, TEX1)
                {
                    const uint32_t n = code & 0xFFFFu;
                    for (uint32_t k = 2; k <= n; ++k) // after the GIF tag: (value, register) pairs
                    {
                        const uint32_t pair = at + k * 16u;
                        const uint64_t value = rd64(rdram, pair);
                        const uint32_t reg = rd32(rdram, pair + 8u) & 0xFFu;
                        if (reg == 0x06u)
                        {
                            material.tex0 = value;
                            material.textured = true;
                        }
                    }
                    q += 1u + n;
                }
                else if ((cmd & 0x60u) == 0x60u) // UNPACK
                {
                    const uint32_t num = (code >> 16) & 0xFFu;
                    const bool tops = (code & 0x8000u) != 0u;
                    // The material (SetMaterial: UNPACK to VU 0x25): its first quadword scales the lights and ambient
                    // (the microprogram's MUL of vf5-vf9 by VU 37).
                    if (!tops && (code & 0x3FFu) == 0x25u && num >= 1u)
                        for (int c = 0; c < 4; ++c)
                            lighting.material[c] = rdf(rdram, at + 16u + static_cast<uint32_t>(c) * 4u);
                    if (tops && num >= 2u)
                    {
                        const uint32_t header = at + 16u;
                        const uint32_t count = rd32(rdram, header + 16u);
                        const uint32_t type = rd32(rdram, header + 20u);
                        const bool colours = rd32(rdram, header + 24u) != 0u;
                        if (count > 0u && count <= 64u && (type == 3u || type == 4u))
                        {
                            emitStrip(rdram, header + 32u, count, type, colours, lighting, vertices);
                            ++g_stats.strips;
                        }
                        else
                        {
                            ++g_stats.skipped;
                        }
                    }
                    q += 1u + num;
                }
                else
                {
                    if (cmd == 0x14u || cmd == 0x15u) // MSCAL / MSCALF: a new program run, a new group
                    {
                        if (drawn)
                            renderer.beginGroup(++g_group);
                        flush();
                    }
                    else if (cmd == 0x17u) // MSCNT: the same run goes on
                    {
                        flush();
                    }
                    q += 1u;
                }
            }
            flush();
            ++g_stats.visuals;
        }

        void drawVu1Hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t visual = getRegU32(ctx, 4), packet = getRegU32(ctx, 5);
            const uint32_t renderInfo = getRegU32(ctx, 7), program = getRegU32(ctx, 8);
            // DrawVu1's last argument set: no lights in the header (VU 9-21 keep the previous ones).
            const bool lit = getRegU32(ctx, 11) == 0u;
            ++g_stats.calls;
            g_drawVu1(rdram, ctx, runtime);
            // Shadow volumes (RenderInfo + 0x320) and screen-space programs (0xF, FST) stay on VU1 for now.
            const bool shadow = rd32(rdram, renderInfo + 0x320u) != 0u, screenSpace = program == 0xFu;
            const bool drawn = !shadow && !screenSpace;
            g_stats.shadows += shadow ? 1u : 0u;
            g_stats.screenSpace += screenSpace ? 1u : 0u;
            if (getRegU32(ctx, 2) != 0u)
                drawVisual(rdram, visual, packet, drawn, lit);
            else
                ++g_stats.empty;
        }

        // DC_PROBE_TEX=1 (diagnosis): each caller of sceVif1PkAddGsAD that writes a TEX0 with an 8- or 4-bit PSM, once.
        constexpr uint32_t kPkAddGsAD = 0x00121028u;
        PS2Runtime::RecompiledFunction g_addGsAD = nullptr;
        void addGsADProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t reg = getRegU32(ctx, 5);
            const uint64_t value = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[6]));
            const uint32_t psm = static_cast<uint32_t>(value >> 20) & 0x3Fu;
            const bool frame = (reg == 0x4Cu || reg == 0x4Du) &&
                               ((static_cast<uint32_t>(value >> 24) & 0x3Fu) != 0u ||
                                ((value & 0x1FFu) != 0u && (value & 0x1FFu) != 210u));
            if (frame || (reg == 6u || reg == 7u) && (psm == 0x13u || psm == 0x14u || psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu))
            {
                static std::mutex s_mutex;
                static std::vector<uint32_t> s_seen;
                const uint32_t ra = getRegU32(ctx, 31);
                std::lock_guard<std::mutex> lock(s_mutex);
                if (std::find(s_seen.begin(), s_seen.end(), ra) == s_seen.end() && s_seen.size() < 64u)
                {
                    s_seen.push_back(ra);
                    if (frame)
                        std::fprintf(stderr, "[dc:probe] FRAME fbp %u fbw %u psm 0x%x from ra 0x%08x\n",
                                     static_cast<uint32_t>(value & 0x1FFu), static_cast<uint32_t>(value >> 16) & 0x3Fu,
                                     static_cast<uint32_t>(value >> 24) & 0x3Fu, ra);
                    else
                        std::fprintf(stderr, "[dc:probe] TEX0 psm 0x%x tbp 0x%x tbw %u cbp 0x%x from ra 0x%08x\n", psm,
                                     static_cast<uint32_t>(value & 0x3FFFu), static_cast<uint32_t>(value >> 14) & 0x3Fu,
                                     static_cast<uint32_t>(value >> 37) & 0x3FFFu, ra);
                }
            }
            g_addGsAD(rdram, ctx, runtime);
        }

        // DC_PROBE_TEX=1: MGMoveImage(src TEX0 *, rect *, dst TEX0 *, dx, dy, dir) (0x12F210), its first calls.
        PS2Runtime::RecompiledFunction g_moveImage = nullptr;
        void moveImageProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static std::atomic<uint32_t> s_logged{0};
            if (s_logged.fetch_add(1u) < 40u)
            {
                const uint32_t rect = getRegU32(ctx, 5);
                std::fprintf(stderr, "[dc:probe] MGMoveImage src %016llx rect %d,%d %dx%d dst %016llx at %u,%u ra 0x%08x\n",
                             static_cast<unsigned long long>(rd64(rdram, getRegU32(ctx, 4))),
                             static_cast<int>(rd32(rdram, rect)), static_cast<int>(rd32(rdram, rect + 4u)),
                             static_cast<int>(rd32(rdram, rect + 8u)), static_cast<int>(rd32(rdram, rect + 12u)),
                             static_cast<unsigned long long>(rd64(rdram, getRegU32(ctx, 6))), getRegU32(ctx, 7),
                             getRegU32(ctx, 8), getRegU32(ctx, 31));
            }
            g_moveImage(rdram, ctx, runtime);
        }

        // DC_PROBE_TEX=1: DC's 2D sprite functions (packet, CTexture *, const CRect &dst, ...): each (caller, texture)
        // that covers most of the screen (dst at least 512 wide), once.
        struct SpriteHook
        {
            uint32_t address;
            PS2Runtime::RecompiledFunction original;
        };
        SpriteHook g_spriteHooks[] = {{0x0014C3C0u, nullptr}, {0x0014CD80u, nullptr}, {0x0015BD00u, nullptr},
                                      {0x0015C000u, nullptr}, {0x0015C310u, nullptr}, {0x0015C630u, nullptr},
                                      {0x0015C980u, nullptr}, {0x0015E540u, nullptr}, {0x0015EAD0u, nullptr}};
        void spriteProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pc = ctx->pc;
            PS2Runtime::RecompiledFunction original = nullptr;
            for (const SpriteHook &hook : g_spriteHooks)
                if (hook.address == pc)
                    original = hook.original;
            static std::atomic<bool> s_first{true};
            if (s_first.exchange(false))
                std::fprintf(stderr, "[dc:probe] sprite hooks called (first 0x%06x)\n", pc);
            const uint32_t texture = getRegU32(ctx, 5), rect = getRegU32(ctx, 6);
            const int32_t width = static_cast<int32_t>(rd32(rdram, rect + 8u));
            if (texture != 0u && width >= 512)
            {
                static std::mutex s_mutex;
                static std::vector<uint64_t> s_seen;
                const uint64_t tex0 = rd64(rdram, texture + 0x28u);
                const uint64_t key = static_cast<uint64_t>(getRegU32(ctx, 31)) << 32 ^ tex0;
                std::lock_guard<std::mutex> lock(s_mutex);
                if (std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end() && s_seen.size() < 64u)
                {
                    s_seen.push_back(key);
                    std::fprintf(stderr, "[dc:probe] sprite 0x%06x from ra 0x%08x: tbp %u tbw %u psm 0x%x cbp %u, dst %d,%d %dx%d\n",
                                 pc, getRegU32(ctx, 31), static_cast<uint32_t>(tex0 & 0x3FFFu),
                                 static_cast<uint32_t>(tex0 >> 14) & 0x3Fu, static_cast<uint32_t>(tex0 >> 20) & 0x3Fu,
                                 static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu, static_cast<int>(rd32(rdram, rect)),
                                 static_cast<int>(rd32(rdram, rect + 4u)), width, static_cast<int>(rd32(rdram, rect + 12u)));
                }
            }
            // DC_SKIP_TBP=<block>: sprites of that texture are not drawn (diagnosis).
            static const uint32_t s_skip = [] { const char *v = std::getenv("DC_SKIP_TBP"); return v ? static_cast<uint32_t>(std::atoi(v)) : ~0u; }();
            if (texture != 0u && (rd64(rdram, texture + 0x28u) & 0x3FFFu) == s_skip)
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            if (original)
                original(rdram, ctx, runtime);
        }

        // DC_PROBE_TEX=1: ClsMes::MakeFukidashi_sub (0x150F50): the balloon's fields (+0x38 type, +0x3C..+0x50 window
        // corners and size, +0x58 tail flag, +0x78 / +0x80 / +0x88 tail points, +0x90 opening), every 120th call.
        PS2Runtime::RecompiledFunction g_fukidashiSub = nullptr;
        void fukidashiProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static std::atomic<uint32_t> s_calls{0};
            if (s_calls.fetch_add(1u) % 120u == 0u)
            {
                const uint32_t mes = getRegU32(ctx, 4);
                std::fprintf(stderr, "[dc:probe] fukidashi:");
                for (uint32_t off = 0x38u; off <= 0x8Cu; off += 4u)
                    std::fprintf(stderr, " %02x=%d", off, static_cast<int32_t>(rd32(rdram, mes + off)));
                std::fprintf(stderr, " 90=%.3f\n", rdf(rdram, mes + 0x90u));
            }
            g_fukidashiSub(rdram, ctx, runtime);
        }

        // DC_PROBE_TEX=1: RollPos(float angle, float *centre, float *point, float *out) (0x14C690), its first calls.
        PS2Runtime::RecompiledFunction g_rollPos = nullptr;
        void rollPosProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static std::atomic<uint32_t> s_logged{0};
            const uint32_t centre = getRegU32(ctx, 4), point = getRegU32(ctx, 5), out = getRegU32(ctx, 6);
            const float angle = ctx->f[12];
            const float cx = rdf(rdram, centre), cy = rdf(rdram, centre + 4u), px = rdf(rdram, point),
                        py = rdf(rdram, point + 4u);
            g_rollPos(rdram, ctx, runtime);
            if (s_logged.fetch_add(1u) < 24u)
                std::fprintf(stderr, "[dc:probe] RollPos angle %.4f centre %.1f,%.1f point %.1f,%.1f -> %.1f,%.1f\n", angle,
                             cx, cy, px, py, rdf(rdram, out), rdf(rdram, out + 4u));
        }

        bool g_enabled = false;
    }

    bool enabled()
    {
        return g_enabled;
    }

    void install(PS2Runtime &runtime)
    {
        // DC_NO_DOF=1 (diagnosis): DepthOfField (0x1652C0, the cutscenes' blur behind the focus plane) does nothing.
        if (const char *noDof = std::getenv("DC_NO_DOF"); noDof && noDof[0] == '1')
            runtime.registerFunction(0x001652C0u, [](uint8_t *, R5900Context *ctx, PS2Runtime *) {
                ctx->pc = getRegU32(ctx, 31);
            });
        if (const char *probe = std::getenv("DC_PROBE_TEX"); probe && probe[0] == '1')
        {
            if ((g_addGsAD = runtime.lookupFunction(kPkAddGsAD)))
                runtime.registerFunction(kPkAddGsAD, &addGsADProbe);
            if ((g_moveImage = runtime.lookupFunction(0x0012F210u)))
                runtime.registerFunction(0x0012F210u, &moveImageProbe);
            if ((g_rollPos = runtime.lookupFunction(0x0014C690u)))
                runtime.registerFunction(0x0014C690u, &rollPosProbe);
            if ((g_fukidashiSub = runtime.lookupFunction(0x00150F50u)))
                runtime.registerFunction(0x00150F50u, &fukidashiProbe);
            for (SpriteHook &hook : g_spriteHooks)
                if ((hook.original = runtime.lookupFunction(hook.address)))
                    runtime.registerFunction(hook.address, &spriteProbe);
        }
        const char *on = std::getenv("DC_NATIVE3D");
        if (!(on && on[0] == '1'))
            return;
        g_drawVu1 = runtime.lookupFunction(kVisualDrawVu1);
        if (!g_drawVu1)
        {
            std::fprintf(stderr, "[dc:native] CVisualVu1::DrawVu1 not found\n");
            return;
        }
        runtime.registerFunction(kVisualDrawVu1, &drawVu1Hook);
        ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
        renderer.setEnabled(true);
        renderer.beginFrame(screen());
        // DC_NATIVE3D_VU1=1 keeps VU1 drawing them too (to compare with PS2X_NATIVE_VIEW=only).
        if (const char *keep = std::getenv("DC_NATIVE3D_VU1"); !(keep && keep[0] == '1'))
        {
            ps2x::native::setVif1Observer(&observe);
            ps2x::native::setVu1Gate(&gate);
            // In the GS stream where VU1's output would have been: after the textures the same chain uploads, sorted
            // with the GS draws around it.
            renderer.setStreamDraws(true);
        }
        g_enabled = true;
        std::fprintf(stderr, "[dc:native] 3D models drawn natively from the game's VU data (lit as the microprogram lights them)\n");
    }

    // At the game's frame end (dc_overrides.cpp's native frame end): publishes the frame built and starts the next.
    void publish()
    {
        if (!g_enabled)
            return;
        ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
        static uint32_t s_frames = 0;
        if (++s_frames % 300u == 0u)
        {
            std::fprintf(stderr, "[dc:native] per 300 frames: %llu calls (%llu empty, %llu without data), %llu visuals, "
                                 "%llu strips, %llu triangles, %llu skipped; left to VU1: %llu shadows, %llu screen-space\n",
                         static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.empty),
                         static_cast<unsigned long long>(g_stats.noData),
                         static_cast<unsigned long long>(g_stats.visuals), static_cast<unsigned long long>(g_stats.strips),
                         static_cast<unsigned long long>(g_stats.triangles), static_cast<unsigned long long>(g_stats.skipped),
                         static_cast<unsigned long long>(g_stats.shadows), static_cast<unsigned long long>(g_stats.screenSpace));
            std::fprintf(stderr, "[dc:native] gate: %llu MSCALs, %llu with a decision, %llu drawn here; observer: "
                                 "%llu pieces, %llu V4-32 unpacks, %llu model strips; last TOPS tag 0x%08x, last PRMODE 0x%x\n",
                         static_cast<unsigned long long>(g_gateCalls.exchange(0)),
                         static_cast<unsigned long long>(g_gateWithModel.exchange(0)),
                         static_cast<unsigned long long>(g_gateSkipped.exchange(0)),
                         static_cast<unsigned long long>(g_pieces.exchange(0)),
                         static_cast<unsigned long long>(g_unpacks6c.exchange(0)),
                         static_cast<unsigned long long>(g_modelUnpacks.exchange(0)), g_lastTagHigh.load(),
                         g_lastPrmode.load());
            g_stats = Stats{};
        }
        renderer.endFrame();
        renderer.beginFrame(screen());
    }

    // DC_PROBE_TEX=1 (diagnosis): the texture manager's (0x1C75870) textures, every 600 frames: count at +0, entries
    // of 0x50 bytes from +0x10F8, each with its TEX0 at +0x28.
    void probeTextures(const uint8_t *rdram)
    {
        static const bool on = [] { const char *v = std::getenv("DC_PROBE_TEX"); return v && v[0] == '1'; }();
        static uint32_t s_frames = 0;
        if (!on || ++s_frames % 600u != 0u)
            return;
        const uint32_t manager = 0x01C75870u, count = rd32(rdram, manager);
        std::fprintf(stderr, "[dc:probe] frame %u: %u textures\n", s_frames, count);
        for (uint32_t i = 0; i < count && i < 512u; ++i)
        {
            const uint32_t entry = manager + 0x10F8u + i * 0x50u;
            const uint64_t tex0 = rd64(rdram, entry + 0x28u);
            char name[33] = {};
            for (uint32_t k = 0; k < 32u; ++k)
            {
                const char c = static_cast<char>(rdram[(entry + k) & 0x1FFFFFFu]);
                name[k] = (c >= 32 && c < 127) ? c : '.';
            }
            std::fprintf(stderr, "[dc:probe]  %3u tbp %5u tbw %2u psm 0x%02x tw %u th %u cbp %5u  %s\n", i,
                         static_cast<uint32_t>(tex0 & 0x3FFFu), static_cast<uint32_t>(tex0 >> 14) & 0x3Fu,
                         static_cast<uint32_t>(tex0 >> 20) & 0x3Fu, static_cast<uint32_t>(tex0 >> 26) & 0xFu,
                         static_cast<uint32_t>(tex0 >> 30) & 0xFu, static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu, name);
        }
    }
}
