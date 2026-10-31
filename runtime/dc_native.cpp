// Native PC rendering, Dark Cloud's adapter: DC's 3D models drawn by the fork's native renderer
// (ps2x::native::Renderer) from the game's own data, not by VU1 and the GS.
//
// Nothing here reads a packet the game builds for the PS2 hardware (no VIF / GIF / GS data): every draw comes from
// the game's own structures, as its EE code holds them.
// - CVisualVu1::DrawVu1 (0x135000) is where DC draws a model piece: the visual, its local-to-world matrix (a2), its
//   RenderInfo (a3: the camera's matrices, the lights, shadow and fog switches) and the VU1 program (t0).
// - The geometry is the model's MDT, the data DC builds the piece's VU data from (CreateVUdataFromMDT 0x135AA0,
//   CreateVUdataFromMDTRemake 0x135E50, hooked to note which MDT a visual is made of; CVisualMDTVu1 also keeps it at
//   + 0x20): positions, normals, texture coordinates and colours, and a list of primitives (strip or triangle list,
//   count, material) of index tuples into them.
// - A material's texture is the CTextureManager record its name finds (as CreateVUdataFromMDT finds it), decoded from
//   the record's own pixels and palette in main memory.
// - An object's alpha test, Z write and translucency are the values CFrameVu1::DrawVu1 hands SetGsReg3 (0x129360).
// - The transform, the lights and the texture coordinate scale are DrawVu1's own computation (its header) redone from
//   RenderInfo; the lighting is the VU1 program's formula.
// VU1 is told to skip every program (setVu1Gate): it runs no microcode. The native batches are numbered as the
// programs VU1 skips (renderer groups), one per primitive, as CreateVUdataFromMDT starts one program per primitive.
//
// DC_NATIVE3D=1 turns it on.

#include "ps2_runtime.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/native/native_renderer.h"
#include "runtime/native/native_vif1.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dc::native
{
    namespace
    {
        constexpr uint32_t kVisualDrawVu1 = 0x00135000u;      // CVisualVu1::DrawVu1(uint *, float (*)[4], RenderInfo *, ...)
        constexpr uint32_t kCreateFromMdt = 0x00135AA0u;      // CVisualVu1::CreateVUdataFromMDT(uint *, uint *, int)
        constexpr uint32_t kCreateFromMdtRemake = 0x00135E50u; // CVisualVu1::CreateVUdataFromMDTRemake(uint *, uint *, int)
        constexpr uint32_t kSetGsReg3 = 0x00129360u;          // SetGsReg3(uint *, ulong *TEST, ulong *ZBUF, ulong *ALPHA)
        constexpr uint32_t kVtVisualMdtVu1 = 0x002A11A0u;     // __vt__13CVisualMDTVu1
        constexpr uint32_t kTextureManager = 0x01C75870u;     // the CTextureManager every MDT material's texture is in
        PS2Runtime::RecompiledFunction g_drawVu1 = nullptr, g_createFromMdt = nullptr, g_createFromMdtRemake = nullptr,
                                       g_setGsReg3 = nullptr;
        uint64_t g_group = 0;

        struct Stats
        {
            uint64_t calls = 0, noModel = 0, primitives = 0, triangles = 0, shadows = 0, screenSpace = 0;
            uint64_t ownTextures = 0, noTexture = 0;
        } g_stats;
        std::atomic<uint64_t> g_gateSkipped{0};

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

        // DC's matrices as its VU0 code keeps them: four columns of four floats. mul(a, b) = a x b (MulMatrix).
        struct Matrix
        {
            float m[16];
        };
        Matrix load(const uint8_t *rdram, uint32_t address)
        {
            Matrix out;
            for (int i = 0; i < 16; ++i)
                out.m[i] = rdf(rdram, address + static_cast<uint32_t>(i) * 4u);
            return out;
        }
        Matrix mul(const Matrix &a, const Matrix &b)
        {
            Matrix out;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    out.m[c * 4 + r] = a.m[0 * 4 + r] * b.m[c * 4 + 0] + a.m[1 * 4 + r] * b.m[c * 4 + 1] +
                                       a.m[2 * 4 + r] * b.m[c * 4 + 2] + a.m[3 * 4 + r] * b.m[3 + c * 4];
            return out;
        }

        // Every MSCAL VIF1 meets is skipped: VU1 runs no microcode. VIF1 numbers the programs it skips (1, 2, ...)
        // and the native batches carry the same numbers (renderer groups), so they sit where VU1's output would be.
        std::atomic<uint64_t> g_gateByPc[4]{}; // diagnosis: skipped at pc 0, 8 (the models'), at another pc, the last other
        bool gate(const uint8_t *, uint32_t pc)
        {
            g_gateSkipped.fetch_add(1u, std::memory_order_relaxed);
            g_gateByPc[pc == 0u ? 0 : pc == 8u ? 1 : 2].fetch_add(1u, std::memory_order_relaxed);
            if (pc != 0u && pc != 8u)
                g_gateByPc[3].store(pc, std::memory_order_relaxed);
            return true;
        }

        // Which MDT each visual is made of, noted as the game builds its VU data from it.
        std::mutex g_mdtMutex;
        std::unordered_map<uint32_t, uint32_t> g_visualMdt;
        void createFromMdtHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                std::lock_guard<std::mutex> lock(g_mdtMutex);
                g_visualMdt[getRegU32(ctx, 4)] = getRegU32(ctx, 6);
            }
            g_createFromMdt(rdram, ctx, runtime);
        }
        void createFromMdtRemakeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                std::lock_guard<std::mutex> lock(g_mdtMutex);
                g_visualMdt[getRegU32(ctx, 4)] = getRegU32(ctx, 6);
            }
            g_createFromMdtRemake(rdram, ctx, runtime);
        }
        uint32_t mdtOf(const uint8_t *rdram, uint32_t visual)
        {
            if (rd32(rdram, visual + 8u) == kVtVisualMdtVu1 && rd32(rdram, visual + 0x20u) != 0u)
                return rd32(rdram, visual + 0x20u);
            std::lock_guard<std::mutex> lock(g_mdtMutex);
            const auto it = g_visualMdt.find(visual);
            return it == g_visualMdt.end() ? 0u : it->second;
        }

        // The object state CFrameVu1::DrawVu1 hands SetGsReg3 just before it draws its visual: TEST (alpha test
        // reference), ZBUF (Z write) and ALPHA (translucency), the game's own values.
        struct ObjectState
        {
            bool valid = false;
            uint64_t test = 0, zbuf = 0, alpha = 0;
        } g_objectState;
        void setGsReg3Hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_objectState.valid = true;
            g_objectState.test = rd64(rdram, getRegU32(ctx, 5));
            g_objectState.zbuf = rd64(rdram, getRegU32(ctx, 6));
            g_objectState.alpha = rd64(rdram, getRegU32(ctx, 7));
            g_setGsReg3(rdram, ctx, runtime);
        }

        void applyObjectState(const ObjectState &state, ps2x::native::Material &material)
        {
            material.blend = ps2x::native::Material::Blend::Alpha;
            material.depthWrite = true;
            material.alphaTest = false;
            if (!state.valid)
                return;
            material.blend = ps2x::native::blendOf(state.alpha);
            material.depthWrite = ((state.zbuf >> 32) & 1u) == 0u;
            // TEST: ATE, ATST (5 GEQUAL, 6 GREATER), AREF; a failing pixel kept out entirely (AFAIL KEEP) is a cut-out.
            const uint32_t ate = state.test & 1u, atst = (state.test >> 1) & 7u, aref = (state.test >> 4) & 0xFFu;
            const uint32_t afail = (state.test >> 12) & 3u;
            if (ate != 0u && afail == 0u && (atst == 5u || atst == 6u))
            {
                const uint32_t ref = atst == 5u ? (aref > 0u ? aref - 1u : 0u) : aref;
                material.alphaTest = atst == 6u || aref > 0u;
                material.alphaRef = static_cast<uint8_t>(std::min<uint32_t>(ref, 255u));
            }
        }

        // The VU1 program's lighting (image 8c7dbae50f1ab651, its setup at pc 0x50; f256fcac190325a3 for the models):
        // DrawVu1 puts RenderInfo's four light directions (+ 0x110..+ 0x130, one light per component) at VU 9-11 and
        // the local-to-world matrix's first three columns, normalised, at VU 5-7; their products are the lights in the
        // model's space (vf2-vf4). Per vertex, d = max(0, vf2 n.x + vf3 n.y + vf4 n.z) and the colour is ambient
        // (+ 0x190) + the lights' colours (+ 0x150..+ 0x180) . d, each scaled by the material's first quadword (VU 37),
        // kept to 254. Texture coordinates are scaled by + 0x1A0's x. A draw with its last argument set reuses the
        // lights of the draw before (VU 9-21 keep them).
        struct Lights
        {
            float dirs[3][4]{};    // RenderInfo + 0x110, + 0x120, + 0x130
            float colour[4][4]{};  // + 0x150..+ 0x180
            float ambient[4]{};    // + 0x190
            float uvScale = 1.0f;  // + 0x1A0 x
        } g_lights;

        struct Lighting
        {
            float dir[3][4]{};
            float colour[4][4]{};
            float ambient[4]{};
            float material[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        };

        Lighting lightingFor(const Lights &lights, const Matrix &localToWorld)
        {
            Lighting l;
            for (int j = 0; j < 3; ++j)
            {
                const float *column = &localToWorld.m[j * 4];
                const float length = std::sqrt(column[0] * column[0] + column[1] * column[1] + column[2] * column[2]);
                const float inverse = length > 0.0f ? 1.0f / length : 0.0f;
                for (int c = 0; c < 4; ++c)
                    l.dir[j][c] = (lights.dirs[0][c] * column[0] + lights.dirs[1][c] * column[1] +
                                   lights.dirs[2][c] * column[2]) *
                                  inverse;
            }
            std::memcpy(l.colour, lights.colour, sizeof(l.colour));
            std::memcpy(l.ambient, lights.ambient, sizeof(l.ambient));
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

        // The microprogram clips each triangle to its clip box (CLIPW against the clip matrix, |x|, |y|, |z| <= w;
        // the clipping routine at 0x1C38 of f256fcac190325a3) and draws what is inside. The renderer clamps depth
        // instead of clipping it and keeps GS Z to 24 bits: here triangles are clipped to the near and far planes
        // (z = -w, z = w) of that clip space, the new vertices interpolated in the object's space (clip coordinates
        // are linear in it). x and y are left to the GPU.
        void clipDepth(const Matrix &clip, std::vector<ps2x::native::Vertex> &triangles)
        {
            using ps2x::native::Vertex;
            struct Clipped
            {
                Vertex v;
                float z, w;
            };
            auto toClip = [&](const Vertex &v) {
                const float z = clip.m[0 * 4 + 2] * v.x + clip.m[1 * 4 + 2] * v.y + clip.m[2 * 4 + 2] * v.z + clip.m[3 * 4 + 2];
                const float w = clip.m[0 * 4 + 3] * v.x + clip.m[1 * 4 + 3] * v.y + clip.m[2 * 4 + 3] * v.z + clip.m[3 * 4 + 3];
                return Clipped{v, z, w};
            };
            auto lerp = [](const Clipped &a, const Clipped &b, float t) {
                Clipped c;
                c.v.x = a.v.x + (b.v.x - a.v.x) * t;
                c.v.y = a.v.y + (b.v.y - a.v.y) * t;
                c.v.z = a.v.z + (b.v.z - a.v.z) * t;
                c.v.u = a.v.u + (b.v.u - a.v.u) * t;
                c.v.v = a.v.v + (b.v.v - a.v.v) * t;
                c.v.fog = a.v.fog + (b.v.fog - a.v.fog) * t;
                c.v.q = a.v.q + (b.v.q - a.v.q) * t;
                uint32_t rgba = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const float ca = static_cast<float>((a.v.rgba >> (k * 8)) & 0xFFu);
                    const float cb = static_cast<float>((b.v.rgba >> (k * 8)) & 0xFFu);
                    rgba |= static_cast<uint32_t>(std::clamp(ca + (cb - ca) * t + 0.5f, 0.0f, 255.0f)) << (k * 8);
                }
                c.v.rgba = rgba;
                c.z = a.z + (b.z - a.z) * t;
                c.w = a.w + (b.w - a.w) * t;
                return c;
            };
            // Distance to a plane, inside >= 0: near z + w, far w - z.
            auto nearDistance = [](const Clipped &c) { return c.z + c.w; };
            auto farDistance = [](const Clipped &c) { return c.w - c.z; };
            std::vector<Vertex> out;
            out.reserve(triangles.size());
            Clipped polygon[9], next[9];
            for (size_t i = 0; i + 2 < triangles.size(); i += 3)
            {
                Clipped a = toClip(triangles[i]), b = toClip(triangles[i + 1]), c = toClip(triangles[i + 2]);
                const bool inside = nearDistance(a) >= 0.0f && nearDistance(b) >= 0.0f && nearDistance(c) >= 0.0f &&
                                    farDistance(a) >= 0.0f && farDistance(b) >= 0.0f && farDistance(c) >= 0.0f;
                if (inside)
                {
                    out.push_back(a.v);
                    out.push_back(b.v);
                    out.push_back(c.v);
                    continue;
                }
                int count = 3;
                polygon[0] = a, polygon[1] = b, polygon[2] = c;
                for (int plane = 0; plane < 2 && count > 0; ++plane)
                {
                    int n = 0;
                    for (int k = 0; k < count; ++k)
                    {
                        const Clipped &p = polygon[k], &q = polygon[(k + 1) % count];
                        const float dp = plane == 0 ? nearDistance(p) : farDistance(p);
                        const float dq = plane == 0 ? nearDistance(q) : farDistance(q);
                        if (dp >= 0.0f)
                            next[n++] = p;
                        if ((dp >= 0.0f) != (dq >= 0.0f))
                            next[n++] = lerp(p, q, dp / (dp - dq));
                    }
                    count = n;
                    for (int k = 0; k < count; ++k)
                        polygon[k] = next[k];
                }
                for (int k = 1; k + 1 < count; ++k)
                {
                    out.push_back(polygon[0].v);
                    out.push_back(polygon[k].v);
                    out.push_back(polygon[k + 1].v);
                }
            }
            triangles.swap(out);
        }

        // A CTextureManager record (0x50 bytes from + 0x10F8): + 0 group, + 2 width, + 4 height, + 6 kind (1: 8-bit
        // with a palette, 4: 32-bit), + 8 name, + 0x28 TEX0, + 0x38 the levels' pixels, + 0x48 the palette, + 0x4C
        // set when its 8-bit texels are stored pre-swizzled and sent as a CT32 image of half the width.
        // SearchTextureName (0x131180): from the last record down, a record whose name matches.
        uint32_t textureRecord(const uint8_t *rdram, uint32_t name)
        {
            const int32_t count = static_cast<int32_t>(rd32(rdram, kTextureManager));
            for (int32_t i = std::min(count, 1023); i >= 0; --i)
            {
                const uint32_t record = kTextureManager + 0x10F8u + static_cast<uint32_t>(i) * 0x50u;
                uint32_t k = 0;
                for (; k < 32u; ++k)
                {
                    const uint8_t a = rdram[(name + k) & 0x1FFFFFFu], b = rdram[(record + 8u + k) & 0x1FFFFFFu];
                    if (a != b || a == 0u)
                        break;
                }
                if (k < 32u && rdram[(name + k) & 0x1FFFFFFu] == 0u && rdram[(record + 8u + k) & 0x1FFFFFFu] == 0u &&
                    rdram[(record + 8u) & 0x1FFFFFFu] != 0u)
                    return record;
            }
            return 0u;
        }

        std::vector<uint8_t> g_unswizzle; // a GS-layout scratch for assets stored pre-swizzled
        std::unordered_map<uint32_t, std::pair<uint64_t, std::shared_ptr<const ps2x::native::TextureCache::Texture>>> g_decoded;
        uint64_t g_textureVersion = 0;

        uint64_t contentHash(const uint8_t *rdram, uint32_t address, uint32_t bytes, uint64_t hash)
        {
            for (uint32_t i = 0; i + 4u <= bytes; i += 4u)
                hash = (hash ^ rd32(rdram, address + i)) * 0x100000001B3ull;
            return hash;
        }

        // A record's texture, decoded from its pixels and palette in main memory (8-bit / 4-bit with a CT32 palette as
        // the game sends it: CSM1 entry order, CSA; CT32). An identical asset keeps its decoded texture.
        std::shared_ptr<const ps2x::native::TextureCache::Texture> recordTexture(const uint8_t *rdram, uint32_t record)
        {
            const uint32_t w = rd32(rdram, record) >> 16, h = rd32(rdram, record + 4u) & 0xFFFFu;
            const uint32_t kind = rd32(rdram, record + 4u) >> 16;
            const uint64_t tex0 = rd64(rdram, record + 0x28u);
            const uint32_t pixels = rd32(rdram, record + 0x38u);
            const uint32_t palette = kind == 1u ? rd32(rdram, record + 0x48u) : 0u;
            const bool sentAs32 = kind == 1u && rd32(rdram, record + 0x4Cu) != 0u;
            const uint32_t bytes = (kind == 1u ? 1u : 4u) * w * h; // ReloadTexture's level 0: kind x w x h / 16 qwords
            const uint32_t tbp = tex0 & 0x3FFFu, tbw = std::max<uint32_t>((tex0 >> 14) & 0x3Fu, 1u);
            const uint32_t psm = (tex0 >> 20) & 0x3Fu;
            const uint32_t tw = (tex0 >> 26) & 0xFu, th = (tex0 >> 30) & 0xFu;
            const uint32_t cpsm = (tex0 >> 51) & 0xFu, csa = (tex0 >> 56) & 0x1Fu;
            if (pixels == 0u || bytes == 0u || (psm != 0x13u && psm != 0x14u && psm != 0x00u) ||
                (psm != 0x00u && (cpsm != 0u || palette == 0u)) || tw > 10u || th > 10u)
                return nullptr;
            const uint32_t sample = std::min<uint32_t>(bytes, 4096u);
            uint64_t hash = contentHash(rdram, pixels, sample, 0xCBF29CE484222325ull ^ tex0);
            hash = contentHash(rdram, pixels + bytes - sample, sample, hash ^ pixels);
            if (palette != 0u)
                hash = contentHash(rdram, palette, 1024u, hash ^ palette);
            auto &slot = g_decoded[record];
            if (slot.second && slot.first == hash)
                return slot.second;

            auto texture = std::make_shared<ps2x::native::TextureCache::Texture>();
            texture->width = 1u << tw;
            texture->height = 1u << th;
            texture->rgba.assign(static_cast<size_t>(texture->width) * texture->height, 0u);
            texture->key = record;
            texture->version = ++g_textureVersion;
            const uint32_t rowTexels = tbw * 64u;
            const uint32_t bitsPerTexel = psm == 0x13u ? 8u : psm == 0x14u ? 4u : 32u;
            const uint32_t rows = bytes * 8u / (rowTexels * bitsPerTexel);
            auto alpha = [](uint32_t a) { return a >= 0x80u ? 255u : a * 2u; };
            auto colour = [&](uint32_t c) { return (c & 0x00FFFFFFu) | alpha(c >> 24) << 24; };
            // 8-bit texels stored for a CT32 send (half the width): laid out as that image is, read back as 8-bit.
            const uint32_t bw32 = std::max<uint32_t>(tbw / 2u, 1u), width32 = bw32 * 64u;
            const uint32_t rows32 = sentAs32 ? bytes / (width32 * 4u) : 0u;
            if (sentAs32)
            {
                g_unswizzle.resize(4u * 1024u * 1024u);
                for (uint32_t y = 0; y < rows32; ++y)
                    for (uint32_t x = 0; x < width32; ++x)
                        GSMem::WriteCT32(g_unswizzle.data(), tbp, bw32, x, y, rd32(rdram, pixels + (y * width32 + x) * 4u));
            }
            const uint32_t texelRows = sentAs32 ? rows32 * 2u : rows;
            for (uint32_t y = 0; y < texture->height && y < texelRows; ++y)
                for (uint32_t x = 0; x < texture->width && x < rowTexels; ++x)
                {
                    uint32_t texel = 0;
                    if (psm == 0x00u)
                        texel = colour(rd32(rdram, pixels + (y * rowTexels + x) * 4u));
                    else
                    {
                        uint32_t index = 0;
                        if (sentAs32)
                            index = psm == 0x13u ? GSMem::ReadP8(g_unswizzle.data(), tbp, tbw, x, y)
                                                 : GSMem::ReadP4(g_unswizzle.data(), tbp, tbw, x, y);
                        else if (psm == 0x13u)
                            index = rdram[(pixels + y * rowTexels + x) & 0x1FFFFFFu];
                        else
                        {
                            const uint32_t at = y * rowTexels + x;
                            index = (rdram[(pixels + at / 2u) & 0x1FFFFFFu] >> ((at & 1u) * 4u)) & 0xFu;
                        }
                        const uint32_t entry = psm == 0x13u ? (index & 0xE7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1)
                                                            : csa * 16u + index;
                        texel = colour(rd32(rdram, palette + entry * 4u));
                    }
                    texture->rgba[static_cast<size_t>(y) * texture->width + x] = texel;
                }
            slot = {hash, texture};
            return texture;
        }

        // A material (0x60 bytes): its first quadword is the colour the lights and ambient are scaled by, its texture
        // name is at + 0x34. Its record, found once per material.
        std::unordered_map<uint32_t, uint32_t> g_materialRecord;

        // One model piece, from its MDT: + 0x10 positions, + 0x18 normals, + 0x20 colours (none when <= 0), + 0x28
        // the primitives (count at + 8, from + 0x10: type (4 strip, 3 list), vertex count, material (-1: the one
        // before), then per vertex the indices of its position, normal, texture coordinates and colour), + 0x30 texture
        // coordinates (the positions when <= 0), + 0x38 the materials.
        void drawModel(const uint8_t *rdram, uint32_t mdt, const Matrix &localToWorld, uint32_t renderInfo, bool drawn)
        {
            const Matrix clip = mul(mul(load(rdram, renderInfo + 0x230u), load(rdram, renderInfo + 0x10u)), localToWorld);
            const Matrix toScreen = mul(load(rdram, renderInfo + 0x270u), clip);
            Lighting lighting = lightingFor(g_lights, localToWorld);
            const float uvScale = g_lights.uvScale;
            const bool textureMapped = rd32(rdram, renderInfo + 0x320u) == 0u && rd32(rdram, renderInfo + 0x340u) == 0u;

            const uint32_t positions = mdt + rd32(rdram, mdt + 0x10u), normals = mdt + rd32(rdram, mdt + 0x18u);
            const int32_t colourOffset = static_cast<int32_t>(rd32(rdram, mdt + 0x20u));
            const int32_t uvOffset = static_cast<int32_t>(rd32(rdram, mdt + 0x30u));
            const uint32_t uvs = uvOffset > 0 ? mdt + static_cast<uint32_t>(uvOffset) : positions;
            const uint32_t stride = colourOffset > 0 ? 4u : 3u;
            const uint32_t materials = mdt + rd32(rdram, mdt + 0x38u);
            const uint32_t list = mdt + rd32(rdram, mdt + 0x28u);
            const uint32_t primitives = rd32(rdram, list + 8u);

            ps2x::native::Material material;
            material.textured = false;
            material.depthTest = true;
            applyObjectState(g_objectState, material);
            std::shared_ptr<const ps2x::native::TextureCache::Texture> texture;
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            std::vector<ps2x::native::Vertex> vertices;

            uint32_t at = list + 0x10u;
            for (uint32_t p = 0; p < primitives && p < 4096u; ++p)
            {
                const uint32_t type = rd32(rdram, at), count = rd32(rdram, at + 4u);
                const int32_t materialIndex = static_cast<int32_t>(rd32(rdram, at + 8u));
                const uint32_t indices = at + 12u;
                at = indices + count * stride * 4u;
                if (materialIndex >= 0)
                {
                    const uint32_t mat = materials + static_cast<uint32_t>(materialIndex) * 0x60u;
                    for (int c = 0; c < 4; ++c)
                        lighting.material[c] = rdf(rdram, mat + static_cast<uint32_t>(c) * 4u);
                    auto found = g_materialRecord.find(mat);
                    const uint32_t record = found != g_materialRecord.end() ? found->second
                                                                            : (g_materialRecord[mat] = textureRecord(rdram, mat + 0x34u));
                    texture = record != 0u ? recordTexture(rdram, record) : nullptr;
                    material.tex0 = record != 0u ? rd64(rdram, record + 0x28u) : 0u;
                }
                if (count == 0u || count > 65536u)
                    continue;
                // Each primitive is a program run of its own on VU1 (an MSCAL): the renderer group VIF1 skips there.
                renderer.beginGroup(++g_group);
                if (!drawn)
                    continue;
                ++g_stats.primitives;
                auto vertex = [&](uint32_t i) {
                    const uint32_t tuple = indices + i * stride * 4u;
                    const uint32_t pi = rd32(rdram, tuple), ni = rd32(rdram, tuple + 4u), ti = rd32(rdram, tuple + 8u);
                    ps2x::native::Vertex v;
                    v.x = rdf(rdram, positions + pi * 16u);
                    v.y = rdf(rdram, positions + pi * 16u + 4u);
                    v.z = rdf(rdram, positions + pi * 16u + 8u);
                    v.u = rdf(rdram, uvs + ti * 16u) * uvScale;
                    v.v = rdf(rdram, uvs + ti * 16u + 4u) * uvScale;
                    v.rgba = shade(lighting, rdf(rdram, normals + ni * 16u), rdf(rdram, normals + ni * 16u + 4u),
                                   rdf(rdram, normals + ni * 16u + 8u));
                    return v;
                };
                vertices.clear();
                if (type == 4u)
                    for (uint32_t i = 2; i < count; ++i)
                    {
                        ps2x::native::Vertex a = vertex(i - 2), b = vertex(i - 1), c = vertex(i);
                        if (i & 1u)
                            std::swap(a, b);
                        vertices.push_back(a);
                        vertices.push_back(b);
                        vertices.push_back(c);
                    }
                else
                    for (uint32_t i = 0; i + 2 < count; i += 3)
                    {
                        vertices.push_back(vertex(i));
                        vertices.push_back(vertex(i + 1));
                        vertices.push_back(vertex(i + 2));
                    }
                clipDepth(clip, vertices);
                if (vertices.empty())
                    continue;
                material.textured = textureMapped && texture != nullptr;
                g_stats.triangles += vertices.size() / 3u;
                if (material.textured)
                {
                    ++g_stats.ownTextures;
                    renderer.draw(material, toScreen.m, vertices.data(), static_cast<uint32_t>(vertices.size()), texture);
                }
                else
                {
                    g_stats.noTexture += textureMapped ? 1u : 0u;
                    renderer.draw(material, toScreen.m, vertices.data(), static_cast<uint32_t>(vertices.size()));
                }
            }
        }

        void drawVu1Hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t visual = getRegU32(ctx, 4), localToWorld = getRegU32(ctx, 6);
            const uint32_t renderInfo = getRegU32(ctx, 7), program = getRegU32(ctx, 8);
            const bool newLights = getRegU32(ctx, 11) == 0u; // DrawVu1's last argument: 0 sends RenderInfo's lights
            ++g_stats.calls;
            // The game still builds its packet (the GS stream keeps its place: the renderer groups); nothing reads it.
            g_drawVu1(rdram, ctx, runtime);
            const ObjectState state = g_objectState;
            g_objectState.valid = false;
            if (rd32(rdram, visual + 0x18u) == 0u || rd32(rdram, visual + 0x1Cu) == 0u)
                return; // DrawVu1 drew nothing (no VU data)
            if (newLights)
            {
                for (int j = 0; j < 3; ++j)
                    for (int c = 0; c < 4; ++c)
                        g_lights.dirs[j][c] = rdf(rdram, renderInfo + 0x110u + static_cast<uint32_t>(j) * 16u + static_cast<uint32_t>(c) * 4u);
                for (int k = 0; k < 4; ++k)
                    for (int c = 0; c < 4; ++c)
                        g_lights.colour[k][c] = rdf(rdram, renderInfo + 0x150u + static_cast<uint32_t>(k) * 16u + static_cast<uint32_t>(c) * 4u);
                for (int c = 0; c < 4; ++c)
                    g_lights.ambient[c] = rdf(rdram, renderInfo + 0x190u + static_cast<uint32_t>(c) * 4u);
                g_lights.uvScale = rdf(rdram, renderInfo + 0x1A0u);
                if (!(g_lights.uvScale > 0.0f && g_lights.uvScale <= 16.0f))
                    g_lights.uvScale = 1.0f;
            }
            // Shadow volumes (RenderInfo + 0x320) and the screen-space program (0xF) are not drawn yet.
            const bool shadow = rd32(rdram, renderInfo + 0x320u) != 0u, screenSpace = program == 0xFu;
            g_stats.shadows += shadow ? 1u : 0u;
            g_stats.screenSpace += screenSpace ? 1u : 0u;
            const uint32_t mdt = mdtOf(rdram, visual);
            if (mdt == 0u)
            {
                ++g_stats.noModel;
                return;
            }
            g_objectState = state;
            drawModel(rdram, mdt, load(rdram, localToWorld), renderInfo, !shadow && !screenSpace);
            g_objectState.valid = false;
        }

        // DC_PROBE_TEX=1 (diagnosis): DC's 2D sprite functions (packet, CTexture *, const CRect &dst, ...): each
        // (caller, texture) that covers most of the screen (dst at least 512 wide), once.
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

        // DC_PROBE_VU1=1 (diagnosis): calls of the routines that may run VU1 programs other than the models'.
        struct CountHook
        {
            uint32_t address;
            const char *name;
            PS2Runtime::RecompiledFunction original;
            std::atomic<uint64_t> calls;
        };
        CountHook g_countHooks[] = {{0x0013C470u, "CCloth::DrawVu1", nullptr, {}},
                                    {0x00160F20u, "CWater::DrawVu1", nullptr, {}},
                                    {0x00164340u, "CEffect::Draw", nullptr, {}},
                                    {0x00163480u, "CRunEffect::Draw", nullptr, {}},
                                    {0x00163B20u, "CCandleEffect::Draw", nullptr, {}},
                                    {0x0012BA20u, "CSpriteTable::DrawTable", nullptr, {}},
                                    {0x00136290u, "CVisualShadow::DrawVu1", nullptr, {}},
                                    {0x00129400u, "CFrameVu1::DrawVu1", nullptr, {}}};
        void countHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            for (CountHook &hook : g_countHooks)
                if (hook.address == ctx->pc)
                {
                    hook.calls.fetch_add(1u, std::memory_order_relaxed);
                    hook.original(rdram, ctx, runtime);
                    return;
                }
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
            for (SpriteHook &hook : g_spriteHooks)
                if ((hook.original = runtime.lookupFunction(hook.address)))
                    runtime.registerFunction(hook.address, &spriteProbe);
        if (const char *probe = std::getenv("DC_PROBE_VU1"); probe && probe[0] == '1')
            for (CountHook &hook : g_countHooks)
                if ((hook.original = runtime.lookupFunction(hook.address)))
                    runtime.registerFunction(hook.address, &countHook);
        const char *on = std::getenv("DC_NATIVE3D");
        if (!(on && on[0] == '1'))
            return;
        g_drawVu1 = runtime.lookupFunction(kVisualDrawVu1);
        g_createFromMdt = runtime.lookupFunction(kCreateFromMdt);
        g_createFromMdtRemake = runtime.lookupFunction(kCreateFromMdtRemake);
        g_setGsReg3 = runtime.lookupFunction(kSetGsReg3);
        if (!g_drawVu1 || !g_createFromMdt || !g_createFromMdtRemake || !g_setGsReg3)
        {
            std::fprintf(stderr, "[dc:native] a hooked function is missing\n");
            return;
        }
        runtime.registerFunction(kVisualDrawVu1, &drawVu1Hook);
        runtime.registerFunction(kCreateFromMdt, &createFromMdtHook);
        runtime.registerFunction(kCreateFromMdtRemake, &createFromMdtRemakeHook);
        runtime.registerFunction(kSetGsReg3, &setGsReg3Hook);
        ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
        renderer.setEnabled(true);
        renderer.beginFrame(screen());
        ps2x::native::setVu1Gate(&gate);
        // In the GS stream where VU1's output would have been, sorted with the GS draws around it.
        renderer.setStreamDraws(true);
        g_enabled = true;
        std::fprintf(stderr, "[dc:native] 3D models drawn natively from the game's MDT models, RenderInfo and textures; VU1 runs nothing\n");
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
            std::fprintf(stderr, "[dc:native] per 300 frames: %llu DrawVu1 calls (%llu without a known MDT), %llu primitives, "
                                 "%llu triangles, %llu own textures, %llu untextured for lack of one; not drawn: %llu shadows, "
                                 "%llu screen-space; VU1 programs skipped: %llu\n",
                         static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.noModel),
                         static_cast<unsigned long long>(g_stats.primitives),
                         static_cast<unsigned long long>(g_stats.triangles),
                         static_cast<unsigned long long>(g_stats.ownTextures),
                         static_cast<unsigned long long>(g_stats.noTexture),
                         static_cast<unsigned long long>(g_stats.shadows),
                         static_cast<unsigned long long>(g_stats.screenSpace),
                         static_cast<unsigned long long>(g_gateSkipped.exchange(0)));
            std::fprintf(stderr, "[dc:native] groups: %llu numbered here; skipped at pc 0: %llu, pc 8: %llu, other: %llu (last 0x%llx)\n",
                         static_cast<unsigned long long>(g_group), static_cast<unsigned long long>(g_gateByPc[0].exchange(0)),
                         static_cast<unsigned long long>(g_gateByPc[1].exchange(0)),
                         static_cast<unsigned long long>(g_gateByPc[2].exchange(0)),
                         static_cast<unsigned long long>(g_gateByPc[3].load()));
            g_stats = Stats{};
            for (CountHook &hook : g_countHooks)
                if (const uint64_t calls = hook.calls.exchange(0))
                    std::fprintf(stderr, "[dc:probe] %s: %llu calls\n", hook.name, static_cast<unsigned long long>(calls));
        }
        renderer.endFrame();
        renderer.beginFrame(screen());
    }

    // DC_PROBE_TEX=1 (diagnosis): the texture manager's (0x1C75870) records, every 600 frames: count at + 0, records
    // of 0x50 bytes from + 0x10F8, each with its name at + 8 and TEX0 at + 0x28.
    void probeTextures(const uint8_t *rdram)
    {
        static const bool on = [] { const char *v = std::getenv("DC_PROBE_TEX"); return v && v[0] == '1'; }();
        static uint32_t s_frames = 0;
        if (!on || ++s_frames % 600u != 0u)
            return;
        const uint32_t count = rd32(rdram, kTextureManager);
        std::fprintf(stderr, "[dc:probe] frame %u: %u textures\n", s_frames, count);
        for (uint32_t i = 0; i < count && i < 512u; ++i)
        {
            const uint32_t entry = kTextureManager + 0x10F8u + i * 0x50u;
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
