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
#include "runtime/gs/gs_off.h"
#include "runtime/native/native_renderer.h"
#include "runtime/native/native_vif1.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
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
            uint64_t ownTextures = 0, noTexture = 0, gsOrderTextures = 0, cloths = 0, shadowTriangles = 0, waters = 0, staleModel = 0, mdtDiffers = 0, noZWrite = 0, copiedModel = 0, depthOfFields = 0, culled = 0, zPast24 = 0, zNegative = 0, guarded = 0, textureCopies = 0, textureCopiesMissed = 0;
            double maxZ = -1e30, minZ = 1e30;
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
            // The models' GIF tag writes ST, RGBAQ, XYZF2 (REGS 0x412) PACKED: the GS takes Z from bits 4..27 of the
            // FTOI4 word, so Z is the matrices' z / w itself (24 bits), not 16 x it as an XYZ2 would have it.
            s.gsZScale = 1.0f;
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

        // The model a visual is made of. DC builds a visual's VU data from an MDT (CreateVUdataFromMDT /
        // ...Remake: "MDT\0", its size at + 8) and, for most static pieces (the town's map parts), the MDT's memory is
        // used for other data afterwards: the VU data is all that is left in the game, and it is not read here. So the
        // MDT is copied when the game builds from it (its own model data, still valid then), keyed by the VU data it
        // builds; a draw reads the live MDT when it is still there (animated models keep theirs at + 0x20 and rebuild
        // from it), the copy otherwise.
        // Its textures are bound there too: CreateVUdataFromMDT takes each primitive's material texture by name
        // (GetTextureHandle -> SearchTextureName, the newest record of that name; none: handle -1, which GetTexture
        // takes as record 0) and writes that record's TEX0 into the VU data; Remake keeps it. The records are noted
        // the same way when the game builds, per primitive: a draw looking the name up later found another model's
        // newer texture of the same name (the Colonel's face blank once Seth and the Genie were loaded).
        struct Model
        {
            const uint8_t *rdram = nullptr;
            std::shared_ptr<const std::vector<uint8_t>> copy;
            std::shared_ptr<const std::vector<uint32_t>> records; // per primitive, as built (0: no material)
            uint32_t mdt = 0;
            explicit operator bool() const { return mdt != 0u; }
            uint32_t u32(uint32_t address) const
            {
                if (!copy)
                    return rd32(rdram, address);
                const uint32_t offset = address - mdt;
                uint32_t value = 0;
                if (offset <= copy->size() && copy->size() - offset >= 4u)
                    std::memcpy(&value, copy->data() + offset, 4);
                return value;
            }
            float f(uint32_t address) const
            {
                const uint32_t bits = u32(address);
                float value = 0.0f;
                std::memcpy(&value, &bits, 4);
                return value;
            }
            uint8_t u8(uint32_t address) const
            {
                if (!copy)
                    return rdram[address & 0x1FFFFFFu];
                const uint32_t offset = address - mdt;
                return offset < copy->size() ? (*copy)[offset] : 0u;
            }
        };
        constexpr uint32_t kMdtMagic = 0x0054444Du; // "MDT\0"
        std::mutex g_mdtMutex;
        struct MdtCopy
        {
            uint32_t mdt = 0;
            std::shared_ptr<const std::vector<uint8_t>> copy;
            std::shared_ptr<const std::vector<uint32_t>> records;
        };
        std::unordered_map<uint32_t, MdtCopy> g_mdtCopies; // VU data -> its MDT, a copy, its texture records
        uint32_t boundRecord(const uint8_t *rdram, uint32_t name); // GetTexture(GetTextureHandle(name))
        void noteMdt(const uint8_t *rdram, uint32_t data, uint32_t mdt, bool bind)
        {
            const uint32_t size = rd32(rdram, mdt + 8u);
            if (rd32(rdram, mdt) != kMdtMagic || size < 0x40u || size > 0x800000u)
                return;
            auto copy = std::make_shared<std::vector<uint8_t>>(size);
            for (uint32_t i = 0; i < size; ++i)
                (*copy)[i] = rdram[(mdt + i) & 0x1FFFFFFu];
            std::shared_ptr<std::vector<uint32_t>> records;
            if (bind)
            {
                records = std::make_shared<std::vector<uint32_t>>();
                const uint32_t stride = static_cast<int32_t>(rd32(rdram, mdt + 0x20u)) > 0 ? 4u : 3u;
                const uint32_t materials = mdt + rd32(rdram, mdt + 0x38u), list = mdt + rd32(rdram, mdt + 0x28u);
                const uint32_t primitives = rd32(rdram, list + 8u);
                uint32_t at = list + 0x10u;
                for (uint32_t p = 0; p < primitives && p < 4096u && at - mdt < size; ++p)
                {
                    const uint32_t count = rd32(rdram, at + 4u);
                    const int32_t material = static_cast<int32_t>(rd32(rdram, at + 8u));
                    records->push_back(material != -1 ? boundRecord(rdram, materials + static_cast<uint32_t>(material) * 0x60u + 0x34u) : 0u);
                    at += 12u + count * stride * 4u;
                }
            }
            std::lock_guard<std::mutex> lock(g_mdtMutex);
            MdtCopy &entry = g_mdtCopies[data];
            if (bind || !entry.records) // Remake: the bindings stay those of the build
                entry.records = std::move(records);
            entry.mdt = mdt;
            entry.copy = std::move(copy);
        }
        void createFromMdtHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            noteMdt(rdram, getRegU32(ctx, 5), getRegU32(ctx, 6), true);
            g_createFromMdt(rdram, ctx, runtime);
        }
        void createFromMdtRemakeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            noteMdt(rdram, getRegU32(ctx, 5), getRegU32(ctx, 6), false);
            g_createFromMdtRemake(rdram, ctx, runtime);
        }
        Model modelOf(const uint8_t *rdram, uint32_t visual)
        {
            Model model;
            model.rdram = rdram;
            const uint32_t kept = rd32(rdram, visual + 8u) == kVtVisualMdtVu1 ? rd32(rdram, visual + 0x20u) : 0u;
            std::lock_guard<std::mutex> lock(g_mdtMutex);
            const auto it = g_mdtCopies.find(rd32(rdram, visual + 0x18u));
            if (it != g_mdtCopies.end())
                model.records = it->second.records;
            if (kept != 0u && rd32(rdram, kept) == kMdtMagic)
            {
                model.mdt = kept;
                return model;
            }
            if (it == g_mdtCopies.end())
                return model;
            model.mdt = it->second.mdt;
            if (rd32(rdram, model.mdt) == kMdtMagic && rd32(rdram, model.mdt + 8u) == it->second.copy->size() &&
                std::memcmp(rdram + (model.mdt & 0x1FFFFFFu), it->second.copy->data(), 64) == 0)
                return model; // still there: live (vertex animation writes it)
            model.copy = it->second.copy;
            ++g_stats.copiedModel;
            return model;
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
            // DC_FORCE_ZWRITE=1 (diagnosis): Z written whatever the object's ZBUF says.
            static const bool s_forceZWrite = [] { const char *v = std::getenv("DC_FORCE_ZWRITE"); return v && v[0] == '1'; }();
            if (!material.depthWrite)
            {
                ++g_stats.noZWrite;
                if (s_forceZWrite)
                    material.depthWrite = true;
            }
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

        // vertexColour: a model's colour stream (MDT + 0x20, the tuples' fourth index; SetVuData copies its four floats
        // as they are and sets 0x100 in the primitive's header): the program's lighting at 0xBF8 multiplies the lit
        // colour by it, component by component, before the clamp (0xAD0 is the same without it).
        uint32_t shade(const Lighting &l, float nx, float ny, float nz, const float *vertexColour = nullptr)
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
                if (vertexColour)
                    v *= vertexColour[c];
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

        uint32_t boundRecord(const uint8_t *rdram, uint32_t name)
        {
            const uint32_t record = textureRecord(rdram, name);
            return record != 0u ? record : kTextureManager + 0x10F8u; // handle -1: GetTexture's record 0
        }

        std::unordered_map<uint32_t, std::pair<uint64_t, std::shared_ptr<const ps2x::native::TextureCache::Texture>>> g_decoded;
        uint64_t g_textureVersion = 0;

        // The 8-bit textures converted offline from the user's disc (tools/dc_texconv.py: the TIM2 pictures of DC's IMG
        // archives, plain on the disc, which EnterTexture keeps in GS order in the game), keyed by name and the FNV-1a 32
        // of the palette as the disc holds it (EnterTexture copies it into the record as it is). DC_TEXTURES=<file>,
        // default build/textures/dc_textures.bin.
        struct StoredTexture
        {
            uint32_t width = 0, height = 0, offset = 0;
        };
        std::vector<uint8_t> g_store;
        std::unordered_map<std::string, std::vector<StoredTexture>> g_storeIndex;
        std::unordered_map<std::string, std::vector<StoredTexture>> g_storeByName; // every palette of a name
        bool g_storeLoaded = false;

        uint32_t fnv1a32(const uint8_t *rdram, uint32_t address, uint32_t bytes)
        {
            uint32_t hash = 0x811C9DC5u;
            for (uint32_t i = 0; i < bytes; ++i)
                hash = (hash ^ rdram[(address + i) & 0x1FFFFFFu]) * 0x01000193u;
            return hash;
        }

        void loadStore()
        {
            g_storeLoaded = true;
            const char *path = std::getenv("DC_TEXTURES");
            std::ifstream file(path && *path ? path : "build/textures/dc_textures.bin", std::ios::binary);
            if (!file)
            {
                std::fprintf(stderr, "[dc:native] no converted textures (run tools/dc_texconv.py game build/textures/dc_textures.bin): "
                                     "8-bit textures draw untextured\n");
                return;
            }
            g_store.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (g_store.size() < 12u || std::memcmp(g_store.data(), "DCTX", 4) != 0)
            {
                g_store.clear();
                return;
            }
            uint32_t count = 0;
            std::memcpy(&count, g_store.data() + 8, 4);
            for (uint32_t i = 0; i < count && 12u + (i + 1u) * 44u <= g_store.size(); ++i)
            {
                const uint8_t *entry = g_store.data() + 12u + i * 44u;
                const std::string name(reinterpret_cast<const char *>(entry), strnlen(reinterpret_cast<const char *>(entry), 32));
                uint16_t width = 0, height = 0;
                uint32_t paletteHash = 0;
                StoredTexture texture;
                std::memcpy(&width, entry + 32, 2);
                std::memcpy(&height, entry + 34, 2);
                std::memcpy(&paletteHash, entry + 36, 4);
                std::memcpy(&texture.offset, entry + 40, 4);
                texture.width = width;
                texture.height = height;
                if (texture.offset + 1024u + texture.width * texture.height <= g_store.size())
                {
                    g_storeIndex[name + '|' + std::to_string(paletteHash)].push_back(texture);
                    g_storeByName[name].push_back(texture);
                }
            }
            std::fprintf(stderr, "[dc:native] %u converted 8-bit textures loaded\n", count);
        }

        // A palette the game loads over a texture's CLUT (SetClut 0x14C5A0: the message windows' text colour, a 256-entry
        // CT32 palette in main memory sent to the texture's CBP), by CBP, for the frame: a record whose TEX0 points there
        // is decoded with it. Cleared at the frame's end.
        std::unordered_map<uint32_t, uint32_t> g_clutOverride;
        uint64_t contentHash(const uint8_t *rdram, uint32_t address, uint32_t bytes, uint64_t hash);
        uint32_t paletteOf(const uint8_t *rdram, uint32_t record)
        {
            const uint32_t cbp = static_cast<uint32_t>((rd64(rdram, record + 0x28u) >> 37) & 0x3FFFu);
            const auto found = g_clutOverride.find(cbp);
            return found != g_clutOverride.end() ? found->second : rd32(rdram, record + 0x48u);
        }

        // An 8-bit record's texture from the converted store: the record's name, its palette's hash and its size.
        std::shared_ptr<ps2x::native::TextureCache::Texture> storedTexture(const uint8_t *rdram, uint32_t record,
                                                                           uint32_t w, uint32_t h, uint32_t tw, uint32_t th)
        {
            if (!g_storeLoaded)
                loadStore();
            char name[33] = {};
            for (uint32_t k = 0; k < 32u; ++k)
                name[k] = static_cast<char>(rdram[(record + 8u + k) & 0x1FFFFFFu]);
            const uint32_t palette = paletteOf(rdram, record);
            if (palette == 0u)
                return nullptr;
            const uint32_t paletteHash = fnv1a32(rdram, palette, 1024u);
            const StoredTexture *stored = nullptr;
            if (const auto found = g_storeIndex.find(std::string(name) + '|' + std::to_string(paletteHash)); found != g_storeIndex.end())
                for (const StoredTexture &candidate : found->second)
                    if (candidate.width * candidate.height == w * h)
                        stored = &candidate;
            // The record's palette is not the disc's (the game changed it after loading): the converted indices of that
            // name and size, coloured with the record's palette as it is now (plain CT32 in main memory, CSM1 order).
            bool livePalette = false;
            if (!stored)
                if (const auto named = g_storeByName.find(name); named != g_storeByName.end())
                    for (const StoredTexture &candidate : named->second)
                        if (candidate.width * candidate.height == w * h)
                        {
                            stored = &candidate;
                            livePalette = true;
                        }
            static std::unordered_map<std::string, bool> s_logged;
            if ((!stored || livePalette) && !s_logged[std::string(name) + '|' + std::to_string(paletteHash)])
            {
                s_logged[std::string(name) + '|' + std::to_string(paletteHash)] = true;
                std::fprintf(stderr, "[dc:native] texture \"%s\" %ux%u palette %08x not in the store%s\n", name, w, h, paletteHash,
                             stored ? ": its indices with the record's palette" : "");
            }
            if (!stored)
                return nullptr;
            auto texture = std::make_shared<ps2x::native::TextureCache::Texture>();
            texture->width = 1u << tw;
            texture->height = 1u << th;
            texture->rgba.assign(static_cast<size_t>(texture->width) * texture->height, 0u);
            const uint8_t *rgba = g_store.data() + stored->offset, *indices = rgba + 1024;
            uint32_t live[256];
            if (livePalette)
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    const uint32_t c = rd32(rdram, palette + ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) * 4u);
                    const uint32_t a = c >> 24;
                    live[i] = (c & 0x00FFFFFFu) | (a >= 0x80u ? 255u : a * 2u) << 24;
                }
            for (uint32_t y = 0; y < stored->height && y < texture->height; ++y)
                for (uint32_t x = 0; x < stored->width && x < texture->width; ++x)
                {
                    const uint8_t index = indices[y * stored->width + x];
                    uint32_t texel = 0;
                    if (livePalette)
                        texel = live[index];
                    else
                        std::memcpy(&texel, rgba + index * 4u, 4);
                    texture->rgba[static_cast<size_t>(y) * texture->width + x] = texel;
                }
            return texture;
        }

        uint64_t contentHash(const uint8_t *rdram, uint32_t address, uint32_t bytes, uint64_t hash)
        {
            for (uint32_t i = 0; i + 4u <= bytes; i += 4u)
                hash = (hash ^ rd32(rdram, address + i)) * 0x100000001B3ull;
            return hash;
        }

        // A record's texture, decoded from its pixels and palette in main memory (8-bit / 4-bit with a CT32 palette as
        // the game sends it: CSM1 entry order, CSA; CT32). An identical asset keeps its decoded texture. Records whose
        // 8-bit texels are stored in GS memory order (flag + 0x4C: the game sends them as a CT32 image of half the
        // width) are not decoded here: by BaldHuman's rule no GS layout function runs in the game; they are to come
        // from an offline conversion of the disc's texture files, and draw untextured until then.
        std::shared_ptr<const ps2x::native::TextureCache::Texture> recordTexture(const uint8_t *rdram, uint32_t record)
        {
            const uint32_t w = rd32(rdram, record) >> 16, h = rd32(rdram, record + 4u) & 0xFFFFu;
            const uint32_t kind = rd32(rdram, record + 4u) >> 16;
            const uint64_t tex0 = rd64(rdram, record + 0x28u);
            const uint32_t pixels = rd32(rdram, record + 0x38u);
            const uint32_t palette = kind == 1u ? paletteOf(rdram, record) : 0u;
            if (kind == 1u && rd32(rdram, record + 0x4Cu) != 0u)
            {
                // Kept in GS order by the game: from the offline conversion of the disc's pictures.
                const uint32_t tw = (tex0 >> 26) & 0xFu, th = (tex0 >> 30) & 0xFu;
                const uint64_t hash = contentHash(rdram, palette, 1024u, 0x5354u ^ (static_cast<uint64_t>(palette) << 16) ^ tex0);
                auto &slot = g_decoded[record];
                if (slot.second && slot.first == hash)
                    return slot.second;
                auto texture = tw <= 10u && th <= 10u ? storedTexture(rdram, record, w, h, tw, th) : nullptr;
                if (!texture)
                {
                    ++g_stats.gsOrderTextures;
                    return nullptr;
                }
                texture->key = record;
                texture->version = ++g_textureVersion;
                slot = {hash, texture};
                return texture;
            }
            const uint32_t bytes = (kind == 1u ? 1u : 4u) * w * h; // ReloadTexture's level 0: kind x w x h / 16 qwords
            const uint32_t tbw = std::max<uint32_t>((tex0 >> 14) & 0x3Fu, 1u);
            const uint32_t psm = (tex0 >> 20) & 0x3Fu;
            const uint32_t tw = (tex0 >> 26) & 0xFu, th = (tex0 >> 30) & 0xFu;
            const uint32_t cpsm = (tex0 >> 51) & 0xFu, csa = (tex0 >> 56) & 0x1Fu;
            if (pixels == 0u || bytes == 0u || (psm != 0x13u && psm != 0x14u && psm != 0x00u) ||
                (psm != 0x00u && (cpsm != 0u || palette == 0u)) || tw > 10u || th > 10u)
                return nullptr;
            // Up to 256 KiB hashed whole (the game rewrites some in place: the message windows' glyphs), larger ones sampled.
            const uint32_t sample = bytes <= 0x40000u ? bytes : 4096u;
            uint64_t hash = contentHash(rdram, pixels, sample, 0xCBF29CE484222325ull ^ tex0);
            if (sample < bytes)
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
            for (uint32_t y = 0; y < texture->height && y < rows; ++y)
                for (uint32_t x = 0; x < texture->width && x < rowTexels; ++x)
                {
                    uint32_t texel = 0;
                    if (psm == 0x00u)
                        texel = colour(rd32(rdram, pixels + (y * rowTexels + x) * 4u));
                    else
                    {
                        uint32_t index = 0;
                        if (psm == 0x13u)
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

        // What DrawVu1 sets up for a piece, redone from RenderInfo: clip = RenderInfo 0x230 x 0x10 x the local-to-world
        // matrix (the microprogram's clip box), RenderInfo 0x270 from there to GS screen space, the lights in the
        // piece's space, and whether it is textured (PRMODE TME: no shadow, RenderInfo + 0x340 clear).
        struct DrawSetup
        {
            Matrix clip, toScreen;
            Lighting lighting;
            float uvScale = 1.0f;
            bool textureMapped = true;
            // RenderInfo + 0x310 bit 2 (the program's flags at VU 0.z): the program culls a triangle whose screen-space
            // (v2 - v0) x (v1 - v0) has a negative z (ADC set on its last vertex), in its list, strip (winding taken
            // back every other vertex) and clipping loops alike. Models only (drawModel).
            bool cull = false;
            // The guard band. Without RenderInfo + 0x310 bit 1 (the program's clipping loops), the program does not clip:
            // its list and strip loops take each vertex's screen x / w, y / w and w against VU 20 and 21 (RenderInfo +
            // 0x210, + 0x220: FMAND of the signs of VU21 - v and v - VU20, mask 0xD0) and set ADC on a triangle unless
            // all three are strictly inside. Such a triangle is dropped whole, as the GS skips it.
            bool guard = false;
            float guardLow[4] = {}, guardHigh[4] = {};
            // Fog (models: drawModel). DrawVu1 sets PRMODE's FGE when RenderInfo + 0x324 is set (not in the shadow
            // pass), FOGCOL from + 0x338 (24 bits), and puts (+ 0x328, + 0x334, + 0x330, + 0x32C) at VU 22; the program
            // gives each vertex F = max(min(VU22.x + VU22.w / w, VU22.y), VU22.z), w its screen-space w (MADD.w with Q).
            bool fog = false;
            uint32_t fogColour = 0;
            float fogBase = 255.0f, fogMax = 255.0f, fogMin = 0.0f, fogScale = 0.0f;
        };
        DrawSetup drawSetup(const uint8_t *rdram, const Matrix &localToWorld, uint32_t renderInfo)
        {
            // DC_PROBE_SPIKES=1: the program's guard band (VU 20 / 21 = RenderInfo + 0x210 / + 0x220) and flags (+ 0x310)
            // of the first RenderInfos seen.
            static const bool s_probe = [] { const char *e = std::getenv("DC_PROBE_SPIKES"); return e && e[0] == '1'; }();
            static int s_logged = 0;
            if (s_probe && s_logged < 40)
            {
                static uint32_t s_last[40];
                const uint32_t flags = rd32(rdram, renderInfo + 0x310u);
                bool seen = false;
                for (int i = 0; i < s_logged; ++i)
                    seen |= s_last[i] == (renderInfo ^ flags << 1);
                if (!seen)
                {
                    s_last[s_logged++] = renderInfo ^ flags << 1;
                    std::fprintf(stderr, "[dc:guard] RenderInfo %08x flags %x: VU20 (%g %g %g %g) VU21 (%g %g %g %g)\n", renderInfo, flags,
                                 rdf(rdram, renderInfo + 0x210u), rdf(rdram, renderInfo + 0x214u), rdf(rdram, renderInfo + 0x218u),
                                 rdf(rdram, renderInfo + 0x21Cu), rdf(rdram, renderInfo + 0x220u), rdf(rdram, renderInfo + 0x224u),
                                 rdf(rdram, renderInfo + 0x228u), rdf(rdram, renderInfo + 0x22Cu));
                }
            }
            DrawSetup setup;
            setup.clip = mul(mul(load(rdram, renderInfo + 0x230u), load(rdram, renderInfo + 0x10u)), localToWorld);
            setup.toScreen = mul(load(rdram, renderInfo + 0x270u), setup.clip);
            setup.lighting = lightingFor(g_lights, localToWorld);
            setup.uvScale = g_lights.uvScale;
            setup.textureMapped = rd32(rdram, renderInfo + 0x320u) == 0u && rd32(rdram, renderInfo + 0x340u) == 0u;
            static const bool s_noGuard = [] { const char *e = std::getenv("DC_NO_GUARD"); return e && e[0] == '1'; }();
            setup.guard = !s_noGuard && (rd32(rdram, renderInfo + 0x310u) & 2u) == 0u;
            for (uint32_t c = 0; c < 4u; ++c)
            {
                const float a = rdf(rdram, renderInfo + 0x210u + c * 4u), b = rdf(rdram, renderInfo + 0x220u + c * 4u);
                setup.guardLow[c] = std::min(a, b);
                setup.guardHigh[c] = std::max(a, b);
            }
            return setup;
        }

        void readLights(const uint8_t *rdram, uint32_t renderInfo)
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

        // Clipped to the near and far planes, then drawn with the decoded texture (untextured without one).
        // DC_PROBE_SPIKES=1 (diagnosis): a triangle spanning over 1500 screen pixels logs the draw it came from (what
        // the hooks note in g_drawWhat), once per source.
        char g_drawWhat[160] = "";
        void probeSpikes(const DrawSetup &setup, const std::vector<ps2x::native::Vertex> &vertices)
        {
            static const bool s_on = [] { const char *e = std::getenv("DC_PROBE_SPIKES"); return e && e[0] == '1'; }();
            if (!s_on)
                return;
            static std::unordered_map<std::string, int> s_seen;
            for (size_t t = 0; t + 2u < vertices.size(); t += 3u)
            {
                float sx[3], sy[3];
                bool ok = true;
                for (int k = 0; k < 3; ++k)
                {
                    const ps2x::native::Vertex &v = vertices[t + static_cast<size_t>(k)];
                    const float *m = setup.toScreen.m;
                    const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                    if (!(w > 0.0f))
                        ok = false;
                    sx[k] = (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w;
                    sy[k] = (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w;
                }
                if (!ok)
                    continue;
                const float span = std::max({std::fabs(sx[0] - sx[1]), std::fabs(sx[1] - sx[2]), std::fabs(sx[0] - sx[2]),
                                             std::fabs(sy[0] - sy[1]), std::fabs(sy[1] - sy[2]), std::fabs(sy[0] - sy[2])});
                if (span < 1500.0f)
                    continue;
                if (s_seen[g_drawWhat]++ < 2)
                {
                    const ps2x::native::Vertex *v = &vertices[t];
                    std::fprintf(stderr, "[dc:spike] %s: span %.0f, object (%g %g %g) (%g %g %g) (%g %g %g)\n", g_drawWhat, span,
                                 v[0].x, v[0].y, v[0].z, v[1].x, v[1].y, v[1].z, v[2].x, v[2].y, v[2].z);
                }
                return;
            }
        }

        void submit(const DrawSetup &setup, ps2x::native::Material &material,
                    const std::shared_ptr<const ps2x::native::TextureCache::Texture> &texture,
                    std::vector<ps2x::native::Vertex> &vertices)
        {
            probeSpikes(setup, vertices);
            if (setup.guard)
            {
                size_t kept = 0;
                for (size_t t = 0; t + 2u < vertices.size(); t += 3u)
                {
                    bool inside = true;
                    for (int k = 0; k < 3 && inside; ++k)
                    {
                        const ps2x::native::Vertex &v = vertices[t + static_cast<size_t>(k)];
                        const float *m = setup.toScreen.m;
                        const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                        const float x = (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w;
                        const float y = (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w;
                        inside = x > setup.guardLow[0] && x < setup.guardHigh[0] && y > setup.guardLow[1] && y < setup.guardHigh[1] &&
                                 w > setup.guardLow[3] && w < setup.guardHigh[3];
                    }
                    if (!inside)
                    {
                        ++g_stats.guarded;
                        continue;
                    }
                    for (int k = 0; k < 3; ++k)
                        vertices[kept++] = vertices[t + static_cast<size_t>(k)];
                }
                vertices.resize(kept);
            }
            if (setup.cull)
            {
                size_t kept = 0;
                for (size_t t = 0; t + 2u < vertices.size(); t += 3u)
                {
                    float sx[3], sy[3];
                    bool front = true;
                    for (int k = 0; k < 3; ++k)
                    {
                        const ps2x::native::Vertex &v = vertices[t + static_cast<size_t>(k)];
                        const float *m = setup.toScreen.m;
                        const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                        if (!(w > 0.0f))
                        {
                            front = true; // crosses the camera plane: left to the depth clip, as the program's clipper
                            break;
                        }
                        sx[k] = (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w;
                        sy[k] = (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w;
                        if (k == 2)
                            front = (sx[2] - sx[0]) * (sy[1] - sy[0]) - (sy[2] - sy[0]) * (sx[1] - sx[0]) >= 0.0f;
                    }
                    if (!front)
                    {
                        ++g_stats.culled;
                        continue;
                    }
                    for (int k = 0; k < 3; ++k)
                        vertices[kept++] = vertices[t + static_cast<size_t>(k)];
                }
                vertices.resize(kept);
            }
            clipDepth(setup.clip, vertices);
            if (vertices.empty())
                return;
            // DC_PROBE_SPIKES=1 also measures the GS Z the clipped vertices get (z / w), against the 24 bits the
            // renderer keeps (it wraps past them): the largest per 300 frames and how many vertices go past.
            static const bool s_probeZ = [] { const char *e = std::getenv("DC_PROBE_SPIKES"); return e && e[0] == '1'; }();
            if (s_probeZ)
                for (const ps2x::native::Vertex &v : vertices)
                {
                    const float *m = setup.toScreen.m;
                    const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                    const float z = (m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]) / w; // XYZF2: Z = z / w
                    g_stats.maxZ = std::max(g_stats.maxZ, static_cast<double>(z));
                    g_stats.minZ = std::min(g_stats.minZ, static_cast<double>(z));
                    if (z >= 16777216.0f)
                        ++g_stats.zPast24;
                    if (z < 0.0f)
                        ++g_stats.zNegative;
                }
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            material.textured = setup.textureMapped && texture != nullptr;
            g_stats.triangles += vertices.size() / 3u;
            if (material.textured)
            {
                ++g_stats.ownTextures;
                renderer.draw(material, setup.toScreen.m, vertices.data(), static_cast<uint32_t>(vertices.size()), texture);
            }
            else
            {
                g_stats.noTexture += setup.textureMapped ? 1u : 0u;
                renderer.draw(material, setup.toScreen.m, vertices.data(), static_cast<uint32_t>(vertices.size()));
            }
        }

        // A material's colour (its first quadword, VU 37 for the program) and its texture record.
        uint32_t materialRecord(const uint8_t *rdram, uint32_t mat)
        {
            auto found = g_materialRecord.find(mat);
            return found != g_materialRecord.end() ? found->second : (g_materialRecord[mat] = textureRecord(rdram, mat + 0x34u));
        }
        // The same for a model's material (its name read through the model: a copy when the MDT is gone), keyed by
        // the name, as SearchTextureName finds a record by it.
        std::unordered_map<std::string, uint32_t> g_namedRecord;
        uint32_t modelMaterialRecord(const uint8_t *rdram, const Model &m, uint32_t mat)
        {
            if (!m.copy)
                return materialRecord(rdram, mat);
            std::string name;
            for (uint32_t k = 0; k < 32u; ++k)
            {
                const char c = static_cast<char>(m.u8(mat + 0x34u + k));
                if (c == '\0')
                    break;
                name.push_back(c);
            }
            auto found = g_namedRecord.find(name);
            if (found != g_namedRecord.end())
                return found->second;
            const int32_t count = static_cast<int32_t>(rd32(rdram, kTextureManager));
            uint32_t record = 0;
            for (int32_t i = std::min(count, 1023); i >= 0 && record == 0u; --i)
            {
                const uint32_t candidate = kTextureManager + 0x10F8u + static_cast<uint32_t>(i) * 0x50u;
                uint32_t k = 0;
                for (; k < name.size() && rdram[(candidate + 8u + k) & 0x1FFFFFFu] == static_cast<uint8_t>(name[k]); ++k)
                {
                }
                if (!name.empty() && k == name.size() && rdram[(candidate + 8u + k) & 0x1FFFFFFu] == 0u)
                    record = candidate;
            }
            return g_namedRecord[name] = record;
        }

        // One model piece, from its MDT: + 0x10 positions, + 0x18 normals, + 0x20 colours (none when <= 0), + 0x28
        // the primitives (count at + 8, from + 0x10: type (4 strip, 3 list), vertex count, material (-1: the one
        // before), then per vertex the indices of its position, normal, texture coordinates and colour), + 0x30 texture
        // coordinates (the positions when <= 0), + 0x38 the materials.
        uint32_t g_fogColour = 0; // FOGCOL as the models' DrawVu1 last set it (RenderInfo + 0x338): the 3D sprites' fog
        void drawModel(const uint8_t *rdram, const Model &m, const Matrix &localToWorld, uint32_t renderInfo, bool drawn)
        {
            const uint32_t mdt = m.mdt;
            DrawSetup setup = drawSetup(rdram, localToWorld, renderInfo);
            setup.cull = (rd32(rdram, renderInfo + 0x310u) & 4u) != 0u;
            setup.fog = rd32(rdram, renderInfo + 0x324u) != 0u && rd32(rdram, renderInfo + 0x320u) == 0u;
            setup.fogColour = rd32(rdram, renderInfo + 0x338u) & 0xFFFFFFu;
            setup.fogBase = rdf(rdram, renderInfo + 0x328u);
            setup.fogMax = rdf(rdram, renderInfo + 0x334u);
            setup.fogMin = rdf(rdram, renderInfo + 0x330u);
            setup.fogScale = rdf(rdram, renderInfo + 0x32Cu);
            static const bool s_noCull = [] { const char *v = std::getenv("DC_NO_CULL"); return v && v[0] == '1'; }();
            if (s_noCull) // diagnosis
                setup.cull = false;
            Lighting &lighting = setup.lighting;
            const float uvScale = setup.uvScale;

            const uint32_t positions = mdt + m.u32(mdt + 0x10u), normals = mdt + m.u32(mdt + 0x18u);
            const int32_t colourOffset = static_cast<int32_t>(m.u32(mdt + 0x20u));
            const int32_t uvOffset = static_cast<int32_t>(m.u32(mdt + 0x30u));
            const uint32_t uvs = uvOffset > 0 ? mdt + static_cast<uint32_t>(uvOffset) : positions;
            const uint32_t stride = colourOffset > 0 ? 4u : 3u;
            const uint32_t colours = colourOffset > 0 ? mdt + static_cast<uint32_t>(colourOffset) : 0u;
            const uint32_t materials = mdt + m.u32(mdt + 0x38u);
            const uint32_t list = mdt + m.u32(mdt + 0x28u);
            const uint32_t primitives = m.u32(list + 8u);

            ps2x::native::Material material;
            material.textured = false;
            material.depthTest = true;
            applyObjectState(g_objectState, material);
            material.fog = setup.fog;
            material.fogColour = setup.fogColour;
            g_fogColour = setup.fogColour;
            std::shared_ptr<const ps2x::native::TextureCache::Texture> texture;
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            std::vector<ps2x::native::Vertex> vertices;

            uint32_t at = list + 0x10u;
            for (uint32_t p = 0; p < primitives && p < 4096u; ++p)
            {
                const uint32_t type = m.u32(at), count = m.u32(at + 4u);
                const int32_t materialIndex = static_cast<int32_t>(m.u32(at + 8u));
                const uint32_t indices = at + 12u;
                at = indices + count * stride * 4u;
                if (materialIndex >= 0)
                {
                    const uint32_t mat = materials + static_cast<uint32_t>(materialIndex) * 0x60u;
                    for (int c = 0; c < 4; ++c)
                        lighting.material[c] = m.f(mat + static_cast<uint32_t>(c) * 4u);
                    const uint32_t record = m.records && p < m.records->size() ? (*m.records)[p] : modelMaterialRecord(rdram, m, mat);
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
                    const uint32_t pi = m.u32(tuple), ni = m.u32(tuple + 4u), ti = m.u32(tuple + 8u);
                    ps2x::native::Vertex v;
                    v.x = m.f(positions + pi * 16u);
                    v.y = m.f(positions + pi * 16u + 4u);
                    v.z = m.f(positions + pi * 16u + 8u);
                    v.u = m.f(uvs + ti * 16u) * uvScale;
                    v.v = m.f(uvs + ti * 16u + 4u) * uvScale;
                    float colour[4];
                    if (stride == 4u)
                    {
                        const uint32_t ci = m.u32(tuple + 12u);
                        for (uint32_t c = 0; c < 4u; ++c)
                            colour[c] = m.f(colours + ci * 16u + c * 4u);
                        // DC_LOG_VCOLOUR=1 (diagnosis): the first colours read, once.
                        static const bool s_log = [] { const char *e = std::getenv("DC_LOG_VCOLOUR"); return e && e[0] == '1'; }();
                        static int s_logged = 0;
                        if (s_log && s_logged < 24)
                        {
                            ++s_logged;
                            std::fprintf(stderr, "[dc:native] vertex colour %u: %g %g %g %g\n", ci, colour[0], colour[1], colour[2], colour[3]);
                        }
                    }
                    v.rgba = shade(lighting, m.f(normals + ni * 16u), m.f(normals + ni * 16u + 4u),
                                   m.f(normals + ni * 16u + 8u), stride == 4u ? colour : nullptr);
                    if (setup.fog)
                    {
                        const float *t = setup.toScreen.m;
                        const float w = t[3] * v.x + t[7] * v.y + t[11] * v.z + t[15];
                        float f = w != 0.0f ? setup.fogBase + setup.fogScale / w : setup.fogMax;
                        f = std::max(std::min(f, setup.fogMax), setup.fogMin);
                        v.fog = std::min(std::max(f, 0.0f), 255.0f) / 255.0f;
                    }
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
                submit(setup, material, texture, vertices);
            }
        }

        void noteShadow(const uint8_t *rdram, const Model &m, const Matrix &localToWorld, uint32_t renderInfo, bool listsOnly);

        // Water. CWater (CWater::DrawVu1 0x160F20) is a height field: rows at + 0, columns at + 4, its current heights
        // at + 8 (rows x columns floats), the corners at + 0x20 (base), + 0x30 (the last row's) and + 0x40 (the last
        // column's), its colour at + 0x90 (RGBA bytes), the height scale at + 0x9C, its visual at + 0x70 (VU data at
        // + 0x88, its size at + 0x8C, that visual's + 0x18 / + 0x1C). CreateVUData (0x1607E0) walks each row from base +
        // row x row step (no height step) and, per column, outputs the point and then sets its height to the column's
        // height x the scale (a column's vertex has the height of the column before it) and steps it along the row; one
        // strip per pair of rows. The game draws it with the screen-space program, textured with "work" (its copy of the
        // frame) at coordinates perturbed by the height differences (+ 0xA0): a refraction the renderer cannot sample
        // yet, so the water is drawn in its colour, alpha blended. CreateVUData is taken over to build nothing (the
        // visual's size 0: DrawVu1 sends no packet and VU1 runs nothing), and the water is drawn where DrawVu1 is
        // called for its visual, with the local-to-world matrix CWater::DrawVu1 hands it.
        constexpr uint32_t kWaterCreateVUData = 0x001607E0u;
        std::mutex g_waterMutex;
        std::unordered_map<uint32_t, uint32_t> g_waterOfVisual;
        void waterCreateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t water = getRegU32(ctx, 4), data = getRegU32(ctx, 5);
            std::memcpy(rdram + ((water + 0x88u) & 0x1FFFFFFu), &data, 4);
            const uint32_t none = 0;
            std::memcpy(rdram + ((water + 0x8Cu) & 0x1FFFFFFu), &none, 4);
            {
                std::lock_guard<std::mutex> lock(g_waterMutex);
                g_waterOfVisual[water + 0x70u] = water;
            }
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
        }

        void drawWater(const uint8_t *rdram, uint32_t water, const Matrix &localToWorld, uint32_t renderInfo)
        {
            const int32_t rows = static_cast<int32_t>(rd32(rdram, water)), columns = static_cast<int32_t>(rd32(rdram, water + 4u));
            const uint32_t heights = rd32(rdram, water + 8u);
            if (rows < 2 || columns < 2 || rows > 256 || columns > 256 || heights == 0u)
                return;
            ++g_stats.waters;
            DrawSetup setup = drawSetup(rdram, localToWorld, renderInfo);
            setup.textureMapped = false;
            float base[3], rowStep[3], columnStep[3];
            for (int i = 0; i < 3; ++i)
            {
                base[i] = rdf(rdram, water + 0x20u + static_cast<uint32_t>(i) * 4u);
                rowStep[i] = (rdf(rdram, water + 0x30u + static_cast<uint32_t>(i) * 4u) - base[i]) / static_cast<float>(rows - 1);
                columnStep[i] = (rdf(rdram, water + 0x40u + static_cast<uint32_t>(i) * 4u) - base[i]) / static_cast<float>(columns - 1);
            }
            rowStep[1] = columnStep[1] = 0.0f;
            const float scale = rdf(rdram, water + 0x9Cu);
            const uint32_t colour = rd32(rdram, water + 0x90u);
            std::vector<ps2x::native::Vertex> grid(static_cast<size_t>(rows) * columns);
            for (int32_t r = 0; r < rows; ++r)
            {
                float point[3] = {base[0] + static_cast<float>(r) * rowStep[0], base[1], base[2] + static_cast<float>(r) * rowStep[2]};
                for (int32_t c = 0; c < columns; ++c)
                {
                    ps2x::native::Vertex &v = grid[static_cast<size_t>(r) * columns + c];
                    v.x = point[0];
                    v.y = point[1];
                    v.z = point[2];
                    v.rgba = colour;
                    point[1] = rdf(rdram, heights + static_cast<uint32_t>(r * columns + c) * 4u) * scale;
                    point[0] += columnStep[0];
                    point[2] += columnStep[2];
                }
            }
            std::vector<ps2x::native::Vertex> vertices;
            for (int32_t r = 0; r + 1 < rows; ++r)
                for (int32_t c = 1; c < columns; ++c)
                {
                    // CreateVUData's strip: row r, then row r + 1, at each column.
                    const ps2x::native::Vertex &a = grid[static_cast<size_t>(r) * columns + c - 1];
                    const ps2x::native::Vertex &b = grid[static_cast<size_t>(r + 1) * columns + c - 1];
                    const ps2x::native::Vertex &d = grid[static_cast<size_t>(r) * columns + c];
                    const ps2x::native::Vertex &e = grid[static_cast<size_t>(r + 1) * columns + c];
                    vertices.insert(vertices.end(), {a, b, d, b, e, d});
                }
            ps2x::native::Material material;
            material.textured = false;
            material.depthTest = true;
            material.depthWrite = false;
            material.blend = ps2x::native::Material::Blend::Alpha;
            submit(setup, material, nullptr, vertices);
        }

        void drawVu1Hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t visual = getRegU32(ctx, 4), localToWorld = getRegU32(ctx, 6);
            const uint32_t renderInfo = getRegU32(ctx, 7), program = getRegU32(ctx, 8);
            const bool newLights = getRegU32(ctx, 11) == 0u; // DrawVu1's last argument: 0 sends RenderInfo's lights
            ++g_stats.calls;
            {
                uint32_t water = 0;
                {
                    std::lock_guard<std::mutex> lock(g_waterMutex);
                    const auto found = g_waterOfVisual.find(visual);
                    water = found == g_waterOfVisual.end() ? 0u : found->second;
                }
                if (water != 0u)
                {
                    g_objectState.valid = false;
                    setReturnS32(ctx, 0);
                    ctx->pc = getRegU32(ctx, 31);
                    drawWater(rdram, water, load(rdram, localToWorld), renderInfo);
                    return;
                }
            }
            // The game still builds its packet (the GS stream keeps its place: the renderer groups); nothing reads it.
            g_drawVu1(rdram, ctx, runtime);
            const ObjectState state = g_objectState;
            g_objectState.valid = false;
            if (rd32(rdram, visual + 0x18u) == 0u || rd32(rdram, visual + 0x1Cu) == 0u)
                return; // DrawVu1 drew nothing (no VU data)
            if (newLights)
                readLights(rdram, renderInfo);
            // Shadow volumes (RenderInfo + 0x320) and the screen-space program (0xF) are not drawn yet.
            const bool shadow = rd32(rdram, renderInfo + 0x320u) != 0u, screenSpace = program == 0xFu;
            g_stats.shadows += shadow ? 1u : 0u;
            g_stats.screenSpace += screenSpace ? 1u : 0u;
            const Model model = modelOf(rdram, visual);
            const uint32_t mdt = model.mdt;
            if (!model)
            {
                ++g_stats.noModel;
                return;
            }
            g_objectState = state;
            std::snprintf(g_drawWhat, sizeof(g_drawWhat), "model visual %08x vtable %08x data %08x mdt %08x %s program %u flags %x",
                          visual, rd32(rdram, visual + 8u), rd32(rdram, visual + 0x18u), mdt, model.copy ? "copy" : "live", program,
                          rd32(rdram, renderInfo + 0x310u));
            drawModel(rdram, model, load(rdram, localToWorld), renderInfo, !shadow && !screenSpace);
            if (shadow)
                noteShadow(rdram, model, load(rdram, localToWorld), renderInfo, false); // a model drawn in the shadow pass
            g_objectState.valid = false;
        }

        // CCloth::DrawVu1 (0x13C470), taken over: no VU data, packet or VU1 program; the cloth drawn from its own
        // simulation grid (rows at + 0x2C, columns at + 0x30; vertex (r, c) at + r * 0x100 + c * 0x10 from + 0x1110 for
        // its position, + 0x5110 its normal, + 0x6110 its texture coordinates), one strip per pair of rows as
        // CreateVUData (0x13C5F0) lays them out (the two rows swapped when the row's flag at + 0x7110 is set), with its
        // material at + 0x60 (texture name at + 0x94). It draws with RenderInfo's lights, as its DrawVu1 call asks.
        constexpr uint32_t kClothDrawVu1 = 0x0013C470u;
        void clothDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t cloth = getRegU32(ctx, 4), localToWorld = getRegU32(ctx, 6), renderInfo = getRegU32(ctx, 7);
            const ObjectState state = g_objectState;
            g_objectState.valid = false;
            setReturnS32(ctx, 0); // nothing added to the game's packet
            ctx->pc = getRegU32(ctx, 31);
            const int32_t rows = static_cast<int32_t>(rd32(rdram, cloth + 0x2Cu));
            const int32_t columns = static_cast<int32_t>(rd32(rdram, cloth + 0x30u));
            if (rows < 2 || columns < 1 || rows > 64 || columns > 16)
                return;
            ++g_stats.cloths;
            readLights(rdram, renderInfo);
            DrawSetup setup = drawSetup(rdram, load(rdram, localToWorld), renderInfo);
            const uint32_t mat = cloth + 0x60u;
            for (int c = 0; c < 4; ++c)
                setup.lighting.material[c] = rdf(rdram, mat + static_cast<uint32_t>(c) * 4u);
            const uint32_t record = materialRecord(rdram, mat);
            auto texture = record != 0u ? recordTexture(rdram, record) : nullptr;
            ps2x::native::Material material;
            material.depthTest = true;
            material.tex0 = record != 0u ? rd64(rdram, record + 0x28u) : 0u;
            applyObjectState(state, material);
            auto vertex = [&](int32_t r, int32_t c) {
                const uint32_t at = cloth + static_cast<uint32_t>(r) * 0x100u + static_cast<uint32_t>(c) * 0x10u;
                ps2x::native::Vertex v;
                v.x = rdf(rdram, at + 0x1110u);
                v.y = rdf(rdram, at + 0x1114u);
                v.z = rdf(rdram, at + 0x1118u);
                v.u = rdf(rdram, at + 0x6110u) * setup.uvScale;
                v.v = rdf(rdram, at + 0x6114u) * setup.uvScale;
                v.rgba = shade(setup.lighting, rdf(rdram, at + 0x5110u), rdf(rdram, at + 0x5114u), rdf(rdram, at + 0x5118u));
                return v;
            };
            std::vector<ps2x::native::Vertex> strip, vertices;
            for (int32_t r = 0; r + 1 < rows; ++r)
            {
                const bool swapped = rd32(rdram, cloth + 0x7110u + static_cast<uint32_t>(r) * 4u) != 0u;
                strip.clear();
                for (int32_t c = 0; c < columns; ++c)
                {
                    strip.push_back(vertex(swapped ? r + 1 : r, c));
                    strip.push_back(vertex(swapped ? r : r + 1, c));
                }
                for (size_t i = 2; i < strip.size(); ++i)
                {
                    ps2x::native::Vertex a = strip[i - 2], b = strip[i - 1];
                    if (i & 1u)
                        std::swap(a, b);
                    vertices.push_back(a);
                    vertices.push_back(b);
                    vertices.push_back(strip[i]);
                }
            }
            std::snprintf(g_drawWhat, sizeof(g_drawWhat), "cloth %08x %dx%d flags %x", cloth, rows, columns, rd32(rdram, renderInfo + 0x310u));
            submit(setup, material, texture, vertices);
        }

        // CVisualShadow::DrawVu1 (0x136290), taken over: no VU data, packet or VU1 program. The shadow (the model's
        // MDT positions through RenderInfo's shadow matrix at + 0x2D0, drawn with the shadow program's colour) is not
        // drawn yet.
        constexpr uint32_t kShadowDrawVu1 = 0x00136290u;

        // Shadows. DC draws them in a pass: MGBeginDrawShadow (0x1308A0) points the GS at a work texture cleared to
        // black; every draw with RenderInfo + 0x320 set projects its geometry through RenderInfo's shadow matrix
        // (+ 0x2D0: clip = RenderInfo 0x230 x 0x10 x 0x2D0 x the local-to-world matrix) and marks the work texture with
        // the shadow program's constant colour (images 69a6 / a750 at 0x1AF8: FTOI0 of 1.0, RGBA 1, 1, 1, 1), no depth
        // test; MGEndDrawShadow (0x130B30, its argument the strength) draws the work texture over the frame read as CT24
        // with TEXA's AEM (black: alpha 0, marked: alpha = the strength) and ALPHA 0x46, Cd x (1 - As). Here each
        // shadow draw's triangles are kept, and the pass's end draws them black with alpha = the strength and an alpha
        // blend (the same Cd x (1 - As)), no depth test. Unlike the work texture, triangles that overlap darken twice.
        // Neither routine's GS side runs: begin's clear covers 432 x 432 of the work texture (fbp 210, the "work" picture
        // Norune's water also copies the frame into), and with no program marking it, end laid the stale rest of it
        // over the frame (Norune in black, with page-sized pieces left: GS pages the backend reloaded).
        struct ShadowDraw
        {
            Matrix toScreen;
            std::vector<ps2x::native::Vertex> vertices;
        };
        std::vector<ShadowDraw> g_shadowDraws;

        // A shadow-mode draw: the MDT's triangles (CreateVUdataShadow's reading: each primitive's index tuples taken
        // three by three as triangles; a strip's when the primitive is one), positions only.
        void noteShadow(const uint8_t *rdram, const Model &m, const Matrix &localToWorld, uint32_t renderInfo, bool listsOnly)
        {
            const uint32_t mdt = m.mdt;
            ShadowDraw draw;
            const Matrix clip = mul(mul(mul(load(rdram, renderInfo + 0x230u), load(rdram, renderInfo + 0x10u)),
                                        load(rdram, renderInfo + 0x2D0u)),
                                    localToWorld);
            draw.toScreen = mul(load(rdram, renderInfo + 0x270u), clip);
            const uint32_t positions = mdt + m.u32(mdt + 0x10u);
            const uint32_t stride = static_cast<int32_t>(m.u32(mdt + 0x20u)) > 0 ? 4u : 3u;
            const uint32_t list = mdt + m.u32(mdt + 0x28u);
            const uint32_t primitives = m.u32(list + 8u);
            auto vertex = [&](uint32_t indices, uint32_t i) {
                const uint32_t pi = m.u32(indices + i * stride * 4u);
                ps2x::native::Vertex v;
                v.x = m.f(positions + pi * 16u);
                v.y = m.f(positions + pi * 16u + 4u);
                v.z = m.f(positions + pi * 16u + 8u);
                return v;
            };
            uint32_t at = list + 0x10u;
            for (uint32_t p = 0; p < primitives && p < 4096u; ++p)
            {
                const uint32_t type = m.u32(at), count = m.u32(at + 4u);
                const uint32_t indices = at + 12u;
                at = indices + count * stride * 4u;
                if (count > 65536u)
                    break;
                if (type == 4u && !listsOnly)
                    for (uint32_t i = 2; i < count; ++i)
                    {
                        ps2x::native::Vertex a = vertex(indices, i - 2), b = vertex(indices, i - 1);
                        if (i & 1u)
                            std::swap(a, b);
                        draw.vertices.push_back(a);
                        draw.vertices.push_back(b);
                        draw.vertices.push_back(vertex(indices, i));
                    }
                else
                    for (uint32_t i = 0; i + 2 < count; i += 3)
                        for (uint32_t k = 0; k < 3; ++k)
                            draw.vertices.push_back(vertex(indices, i + k));
            }
            clipDepth(clip, draw.vertices);
            if (!draw.vertices.empty())
                g_shadowDraws.push_back(std::move(draw));
        }

        // CVisualShadow::DrawVu1, taken over: no VU data, packet or VU1 program; its shadow model (the MDT at + 0x20)
        // noted for the pass.
        void shadowDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t visual = getRegU32(ctx, 4), localToWorld = getRegU32(ctx, 6), renderInfo = getRegU32(ctx, 7);
            g_objectState.valid = false;
            ++g_stats.shadows;
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            Model shadowModel;
            shadowModel.rdram = rdram;
            shadowModel.mdt = rd32(rdram, visual + 0x20u);
            if (shadowModel.mdt != 0u && rd32(rdram, shadowModel.mdt) == kMdtMagic)
                noteShadow(rdram, shadowModel, load(rdram, localToWorld), renderInfo, true);
        }

        void beginShadowHook(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            g_shadowDraws.clear();
            ctx->pc = getRegU32(ctx, 31);
        }

        void endShadowHook(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t strength = getRegU32(ctx, 4) & 0xFFu;
            ctx->pc = getRegU32(ctx, 31);
            ps2x::native::Material material;
            material.textured = false;
            material.blend = ps2x::native::Material::Blend::Alpha;
            material.depthTest = false;
            material.depthWrite = false;
            for (ShadowDraw &draw : g_shadowDraws)
            {
                for (ps2x::native::Vertex &v : draw.vertices)
                    v.rgba = strength << 24; // black, alpha = the strength (0x80 = 1.0)
                g_stats.shadowTriangles += draw.vertices.size() / 3u;
                ps2x::native::Renderer::instance().draw(material, draw.toScreen.m, draw.vertices.data(),
                                                        static_cast<uint32_t>(draw.vertices.size()));
            }
            g_shadowDraws.clear();
        }

        // DepthOfField (0x1652C0: a0 the focus, two depths; a1 its passes; a2 the blur's alpha; a3 a haze's strength),
        // taken over. The game stretches its 640 x 224 frame to half its width in a work texture, and that to half
        // again, then draws them back in strips over what lies behind the focus: Z test GEQUAL, no Z write, at the
        // depths of four points of its table (0x266930) moved to the focus (z, z + 30 for the first pass, alternating
        // column by column; z', z' + 20 for the second) through the world-to-screen matrix (0x1C75570). The GS's copy
        // has none of the native draws: drawn the same way here from the native frame (Material::fromFrame).
        uint64_t g_hazeSeed = 0x2545F4914F6CDD1Dull;
        void depthOfFieldHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t focus = getRegU32(ctx, 4);
            const int32_t passes = static_cast<int32_t>(getRegU32(ctx, 5)), alpha = static_cast<int32_t>(getRegU32(ctx, 6));
            const int32_t haze = static_cast<int32_t>(getRegU32(ctx, 7));
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            const float zScale = renderer.gsZScale();
            if (g_group == 0u || !(zScale > 0.0f))
                return;
            ++g_stats.depthOfFields;
            const Matrix toScreen = load(rdram, 0x01C75570u);
            int32_t depth[4];
            for (uint32_t i = 0; i < 4u; ++i)
            {
                float p[4];
                for (uint32_t k = 0; k < 4u; ++k)
                    p[k] = rdf(rdram, 0x00266930u + i * 16u + k * 4u);
                p[2] = i == 0u ? rdf(rdram, focus) : i == 1u ? rdf(rdram, focus) + 30.0f
                     : i == 2u ? rdf(rdram, focus + 4u) : rdf(rdram, focus + 4u) + 20.0f;
                float out[4];
                for (int r = 0; r < 4; ++r)
                    out[r] = toScreen.m[r] * p[0] + toScreen.m[4 + r] * p[1] + toScreen.m[8 + r] * p[2] + toScreen.m[12 + r] * p[3];
                const float z = out[2] / out[3];
                depth[i] = std::isfinite(z) && std::fabs(z) < 2.1e9f ? static_cast<int32_t>(z) : 0;
            }
            ps2x::native::Material material;
            material.textured = true;
            material.tex0 = 0; // TFX MODULATE, TCC 0: the strips' alpha
            material.frameWidth = 640;
            material.frameHeight = 224;
            material.blend = ps2x::native::Material::Blend::Alpha;
            material.depthTest = true;
            material.depthWrite = false;
            material.clampU = material.clampV = true;
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::vector<ps2x::native::Vertex> strip, triangles;
            auto vertex = [&](float x, float y, int32_t z, float u, float v, uint32_t a) {
                ps2x::native::Vertex out;
                out.x = 1728.0f + x;
                out.y = 1936.0f + y;
                out.z = static_cast<float>(static_cast<uint32_t>(z)) / zScale; // XYZ2's Z, unsigned
                out.u = u;
                out.v = v;
                out.rgba = 0x808080u | (a & 0xFFu) << 24;
                strip.push_back(out);
            };
            auto flush = [&]() {
                for (size_t k = 0; k + 2u < strip.size(); ++k)
                    triangles.insert(triangles.end(), {strip[k], strip[k + 1u], strip[k + 2u]});
                strip.clear();
            };
            // First pass: the half-width copy, 8 rows of 32 lines, 41 columns 16 pixels apart.
            for (int32_t row = 0; row < 8; ++row)
            {
                for (int32_t column = 0; column < 41; ++column)
                {
                    const int32_t z = depth[(column & 1) != 0 ? 1 : 0];
                    vertex(column * 16.0f, row * 32.0f, z, column / 40.0f, row * 32.0f / 224.0f, static_cast<uint32_t>(alpha));
                    vertex(column * 16.0f, row * 32.0f + 32.0f, z, column / 40.0f, (row * 32.0f + 32.0f) / 224.0f, static_cast<uint32_t>(alpha));
                }
                flush();
            }
            material.fromFrame = 1;
            renderer.draw(material, kIdentity, triangles.data(), static_cast<uint32_t>(triangles.size()));
            triangles.clear();
            if (passes <= 1)
                return;
            // Second pass: the quarter-width copy, 14 rows of 16 lines, 21 columns 32 pixels apart; with a haze, each
            // strip's points move sideways by a random walk the game keeps at 0x1D18A00 (21 columns of 15 floats).
            const uint32_t walk = 0x01D18A00u;
            if (haze > 0)
            {
                const float step = rdf(rdram, 0x002A188Cu);
                for (uint32_t column = 0; column < 21u; ++column)
                    for (uint32_t row = 0; row < 15u; ++row)
                    {
                        g_hazeSeed = g_hazeSeed * 6364136223846793005ull + 1442695040888963407ull;
                        const float random = static_cast<float>(g_hazeSeed >> 33) / 2147483648.0f; // 0..1, as rand() / 2^31
                        float value = rdf(rdram, walk + row * 4u + column * 0x3Cu) + step * static_cast<float>(haze) * (random - 0.5f);
                        value = std::min(std::max(value, 0.0f), static_cast<float>(haze));
                        uint32_t bits = 0;
                        std::memcpy(&bits, &value, 4);
                        std::memcpy(rdram + ((walk + row * 4u + column * 0x3Cu) & 0x1FFFFFFu), &bits, 4);
                    }
            }
            const uint32_t secondAlpha = haze > 0 ? 0x80u : static_cast<uint32_t>(alpha);
            for (int32_t row = 0; row < 14; ++row)
            {
                for (uint32_t column = 0; column < 21u; ++column)
                {
                    const int32_t z = depth[(column & 1u) != 0u ? 3 : 2];
                    float top = 0.0f, bottom = 0.0f; // the walk's offsets, in 1/16 pixels
                    if (haze > 0)
                    {
                        top = static_cast<float>(static_cast<int32_t>(rdf(rdram, walk + static_cast<uint32_t>(row) * 4u + column * 0x3Cu)));
                        bottom = static_cast<float>(static_cast<int32_t>(rdf(rdram, walk + static_cast<uint32_t>(row) * 4u + column * 0x3Cu + 4u)));
                    }
                    vertex(column * 32.0f - top / 16.0f, row * 16.0f, z, column / 20.0f, row * 16.0f / 224.0f, secondAlpha);
                    vertex(column * 32.0f - bottom / 16.0f, row * 16.0f + 16.0f, z, column / 20.0f, (row * 16.0f + 16.0f) / 224.0f, secondAlpha);
                }
                flush();
            }
            material.fromFrame = 2;
            renderer.draw(material, kIdentity, triangles.data(), static_cast<uint32_t>(triangles.size()));
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

        // DC_PROBE_FEEDERS=1 (inventory): calls, per 300 frames, of every routine that builds GS packets or drives the GS
        // library (their list: the routines whose recompiled code calls sceVif1Pk*, sceGif*, sceGs* or sceDma*).
        struct FeederHook
        {
            uint32_t address;
            const char *name;
            PS2Runtime::RecompiledFunction original = nullptr;
            std::atomic<uint64_t> calls{0};
        };
        FeederHook g_feeders[] = {
            {0x0012A110u, "DrawVu1__9CFrameVu1FP13sceVif1PacketP10RenderInfo"},
            {0x0012BA20u, "DrawTable__12CSpriteTableFv"},
            {0x0012C800u, "StoreImage__Fv"},
            {0x0012CD90u, "MGBeginFrame__Fv"},
            {0x0012D100u, "MGEndFrame__Fv"},
            {0x0012D9A0u, "MGSetWindowRect__Fv"},
            {0x0012DAE0u, "MGSetWindowRect__F8CRect_i_"},
            {0x0012ED80u, "MGDraw__FP6CFrame"},
            {0x0012EDF0u, "MGSetGsTEST__FP9sceGsTest"},
            {0x0012EEC0u, "MGSetGsZBUF__FP9sceGsZbuf"},
            {0x0012EF90u, "MGSetGsALPHA__FP10sceGsAlpha"},
            {0x0012F060u, "MGSetGsTEXA__FP9sceGsTexa"},
            {0x0012F210u, "MGMoveImage__FP9sceGsTex0RC8CRect_i_P9sceGsTex0iii"},
            {0x0012F430u, "MGStretchMoveImage__FP9sceGsTex0RC8CRect_i_P9sceGsTex0RC8CRect_i"},
            {0x0012FA70u, "MGFillBox__FRC8CRect_i_UcUcUcUc"},
            {0x0012FD30u, "MGClearZBuffer__Fi"},
            {0x00130000u, "MGClearScreen__FUcUcUcUc"},
            {0x001303B0u, "MGDrawShadowFast__FP6CFramePfPf"},
            {0x001304D0u, "MGDrawShadowFast2__FP6CFramePfPf"},
            {0x001305F0u, "MGDrawShadow__FP6CFramePfPf"},
            {0x001308A0u, "MGBeginDrawShadow__F9sceGsTex0"},
            {0x00130B30u, "MGEndDrawShadow__FUc"},
            {0x001320E0u, "EnterFixTexture__15CTextureManagerFPcPUciiiPUciiPUcPUcPUcUli"},
            {0x00132930u, "EnterFixTextureZ__15CTextureManagerFPUc"},
            {0x00133070u, "ReloadTexture__15CTextureManagerFP13sceVif1Packeti"},
            {0x00134BC0u, "DrawVu1__10CVisualVu1FP13sceVif1PacketPA4_fP10RenderInfo11VU1_PR"},
            {0x0013DEA0u, "openGiftag__FP13sceVif1Packet"},
            {0x0013DF00u, "closeGiftag__FP13sceVif1Packet"},
            {0x0013DF40u, "Draw__10CDebugFontFv"},
            {0x00140660u, "MoveImageTest__FP13sceVif1PacketiiiRC8CRect_i_iiiiii"},
            {0x00140DA0u, "SetEnv__FP13sceVif1Packet"},
            {0x0014C220u, "set2DSprite_Start__FP13sceVif1PacketP8CTexture"},
            {0x0014C4F0u, "set2DSprite_End__FP13sceVif1PacketP8CTexture"},
            {0x0014C5A0u, "SetClut__FP13sceVif1PacketP8CTextureP1"},
            {0x0014CD80u, "Myset2DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i_"},
            {0x00150BB0u, "DrawMaru__FP13sceVif1Packetiiiiiiii"},
            {0x00150F50u, "MakeFukidashi_sub__6ClsMesFP13sceVif1Packeti"},
            {0x00151AD0u, "MakeFukidashi__6ClsMesFP13sceVif1Packet"},
            {0x00153310u, "DrawMesWin__6ClsMesFv"},
            {0x00154320u, "VSyncCallBack_Load__Fi"},
            {0x00154770u, "LoadTexture__FPcPUcP8CTextureii"},
            {0x00154950u, "LoadTexture__FP8TM2_headP8CTextureii"},
            {0x0015BC20u, "setAlphaFlag__FP13sceVif1PacketP10sceGsAlpha"},
            {0x0015BD00u, "set2DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_ii"},
            {0x0015C000u, "set2DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i_"},
            {0x0015C310u, "set2DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i_Uc"},
            {0x0015C630u, "set2DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i_Uc"},
            {0x0015C980u, "set2DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i_P6"},
            {0x0015D1B0u, "set3DColSprite__FP13sceVif1PacketPiPiPiPiP6spRGBAP6spRGBAP6spRGB"},
            {0x0015D4B0u, "set3DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_PiPiPiPiP6spR"},
            {0x0015D790u, "set3DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_PiPiP6spRGBA"},
            {0x0015D9A0u, "set3DSpriteFog__FP13sceVif1PacketP8CTextureRC8CRect_i_PiPiPiPiUc"},
            {0x0015DCA0u, "set3DSpriteFog__FP13sceVif1PacketP8CTextureRC8CRect_i_PiPiP6spRG"},
            {0x0015DED0u, "setColSprite__FP13sceVif1PacketPiPiPiPiUcUcUcUc"},
            {0x0015E120u, "set2DSpriteC4__FP13sceVif1PacketRC8CRect_i_P6spRGBAP6spRGBAP6spR"},
            {0x0015E540u, "set2DSprite__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i_ii"},
            {0x0015EAD0u, "set2DSpriteRot__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i"},
            {0x00160F20u, "DrawVu1__6CWaterFP10RenderInfoP13sceVif1PacketP1"},
            {0x00161AC0u, "DrawFire__9CFireOmniFiiP7CCameraPffif"},
            {0x00162580u, "blendTextuer__FP13sceVif1PacketiiiP8CTextureRC8CRect_i_RC8CRect_"},
            {0x00162D80u, "blendTextuerTest__FP13sceVif1PacketiiiRC8CRect_i_P8CTextureRC8CR"},
            {0x00163480u, "Draw__10CRunEffectFv"},
            {0x001652C0u, "DepthOfField__FPfiii"},
            {0x00167170u, "TexAnime__13CTextureAnimeFi"},
            {0x00171210u, "DrawLine__FPiPiUcUcUcUc"},
            {0x001797E0u, "EditLoop__Fv"},
            {0x001831D0u, "set2DSpriteRot__FP13sceVif1PacketP8CTextureRC8CRect_i_RC8CRect_i"},
            {0x0019F9B0u, "setTexAnim__Fv"},
            {0x001AB0F0u, "FishLineDraw__Fi"},
            {0x001B1EF0u, "setItemToReserved__FPciiPcii"},
            {0x001F61E0u, "BattleMenuDraw__Fv"},
            {0x00244C70u, "LangsetLoop__Fv"},
        };
        void feederHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            for (FeederHook &hook : g_feeders)
                if (hook.address == ctx->pc)
                {
                    hook.calls.fetch_add(1u, std::memory_order_relaxed);
                    hook.original(rdram, ctx, runtime);
                    return;
                }
        }

        // DC without the GS (PS2X_GS_RENDERER=none): the frame is the native renderer's alone, drawn by present() in
        // the order the game calls its routines. Its 2D comes from those routines' own arguments: screen-space draws, x
        // and y in GS primitive coordinates (pixels plus DC's XYOFFSET, 1728, 1936), z the GS Z they would write.
        bool g_noGs = false;
        constexpr float kOriginX = 1728.0f, kOriginY = 1936.0f;
        // present() takes a vertex colour as the GS's modulate does (0x80 = 1.0); an untextured GS primitive is its RGB
        // as it is (0xFF = 1.0): flat colours are passed at half.
        uint32_t flatColour(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
        {
            auto half = [](uint32_t c) { return ((c & 0xFFu) * 128u + 127u) / 255u; };
            return half(r) | half(g) << 8 | half(b) << 16 | (a & 0xFFu) << 24;
        }
        void quad(std::vector<ps2x::native::Vertex> &out, float x0, float y0, float x1, float y1, float z, uint32_t rgba,
                  float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f)
        {
            auto vertex = [&](float x, float y, float u, float v) {
                ps2x::native::Vertex out;
                out.x = x;
                out.y = y;
                out.z = z;
                out.u = u;
                out.v = v;
                out.rgba = rgba;
                return out;
            };
            const ps2x::native::Vertex a = vertex(x0, y0, u0, v0), b = vertex(x1, y0, u1, v0), c = vertex(x0, y1, u0, v1),
                                       d = vertex(x1, y1, u1, v1);
            out.insert(out.end(), {a, b, c, b, d, c});
        }
        void drawScreen(const ps2x::native::Material &material, const std::vector<ps2x::native::Vertex> &vertices,
                        std::shared_ptr<const ps2x::native::TextureCache::Texture> texture = nullptr)
        {
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            if (!vertices.empty())
                ps2x::native::Renderer::instance().draw(material, kIdentity, vertices.data(), static_cast<uint32_t>(vertices.size()),
                                                        std::move(texture));
        }

        // MGClearScreen (0x130000, a0..a3 the colour): a sprite over the 640 x 224 frame in that colour, ALPHA 0x2A (Cs:
        // opaque), Z test ALWAYS and Z written (0: the far end).
        void clearScreenHook(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            ps2x::native::Material material;
            material.textured = false;
            material.blend = ps2x::native::Material::Blend::Opaque;
            material.depthTest = false;
            material.depthWrite = true;
            std::vector<ps2x::native::Vertex> vertices;
            quad(vertices, kOriginX, kOriginY, kOriginX + 640.0f, kOriginY + 224.0f, 0.0f,
                 flatColour(getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7)));
            drawScreen(material, vertices);
        }
        // MGClearZBuffer (0x12FD30, a0 the Z): the same sprite with ALPHA 0x6A (Cd: the colour kept), Z written.
        void clearZHook(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            ps2x::native::Material material;
            material.textured = false;
            material.blend = ps2x::native::Material::Blend::Alpha; // As 0: the target's colour
            material.depthTest = false;
            material.depthWrite = true;
            std::vector<ps2x::native::Vertex> vertices;
            quad(vertices, kOriginX, kOriginY, kOriginX + 640.0f, kOriginY + 224.0f,
                 static_cast<float>(getRegU32(ctx, 4)), 0u);
            drawScreen(material, vertices);
        }
        // MGFillBox (0x12FA70: a0 the rectangle, x, y, w, h in 1/16 pixels; a1..a4 the colour): an alpha-blended sprite
        // (PRIM ABE with the game's ALPHA, (Cs - Cd) As + Cd), Z test ALWAYS, Z not written.
        void fillBoxHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t rect = getRegU32(ctx, 4);
            const float x = static_cast<float>(static_cast<int32_t>(rd32(rdram, rect))) / 16.0f;
            const float y = static_cast<float>(static_cast<int32_t>(rd32(rdram, rect + 4u))) / 16.0f;
            const float w = static_cast<float>(static_cast<int32_t>(rd32(rdram, rect + 8u))) / 16.0f;
            const float h = static_cast<float>(static_cast<int32_t>(rd32(rdram, rect + 12u))) / 16.0f;
            ps2x::native::Material material;
            material.textured = false;
            material.blend = ps2x::native::Material::Blend::Alpha;
            material.depthTest = false;
            material.depthWrite = false;
            std::vector<ps2x::native::Vertex> vertices;
            quad(vertices, kOriginX + x, kOriginY + y, kOriginX + x + w, kOriginY + y + h, 0.0f,
                 flatColour(getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8)));
            drawScreen(material, vertices);
        }

        // DC's 2D sprites (set2DSprite and its variants, 0x15BD00..), from their arguments: the CTexture (its record:
        // TEX0 at + 0x28), the destination rectangle (x, y, w, h in DC's 640 x 448 view: GS x = 1728 + x, y = 1936 + y / 2),
        // the source rectangle in texels, the colours. Each is a sprite or a 4-vertex strip with PRIM ABE (the ALPHA
        // the game last set: setAlphaFlag / MGSetGsALPHA), Z test ALWAYS, no Z write. The CRect is (x, y, w, h).
        uint64_t g_alpha2D = 0x44; // (Cs - Cd) As + Cd
        struct Corner
        {
            float x, y; // GS pixels
            float u, v; // texels
            uint32_t rgba;
        };
        uint32_t stackArg(const uint8_t *rdram, R5900Context *ctx, uint32_t index) // the 9th argument on: the caller's stack
        {
            return rd32(rdram, getRegU32(ctx, 29) + index * 8u);
        }
        // Textures the game draws into (its frame pointed at them), each an offscreen target of the renderer: the message
        // balloons' (0x299A78, target 1). A sprite of that record samples the target.
        uint32_t g_balloonRecord = 0;
        // A strip of four corners (v0 v1 v2, v1 v2 v3), textured with the record's texture when there is one.
        void drawStrip(const uint8_t *rdram, uint32_t record, const Corner (&c)[4], bool flat = false)
        {
            if (record != 0u && record == g_balloonRecord)
            {
                ps2x::native::Material material;
                material.textured = true;
                material.fromFrame = 1;
                material.source = 1;
                material.frameWidth = 640;
                material.frameHeight = 224;
                material.blend = ps2x::native::blendOf(g_alpha2D);
                material.depthTest = false;
                material.depthWrite = false;
                std::vector<ps2x::native::Vertex> vertices;
                auto vertex = [&](const Corner &k) {
                    ps2x::native::Vertex v;
                    v.x = k.x;
                    v.y = k.y;
                    v.u = k.u / 640.0f; // the target's pixels are the texture's texels
                    v.v = k.v / 224.0f;
                    v.rgba = k.rgba;
                    return v;
                };
                vertices.insert(vertices.end(), {vertex(c[0]), vertex(c[1]), vertex(c[2]), vertex(c[1]), vertex(c[3]), vertex(c[2])});
                drawScreen(material, vertices);
                return;
            }
            std::shared_ptr<const ps2x::native::TextureCache::Texture> texture = record != 0u ? recordTexture(rdram, record) : nullptr;
            // DC_PROBE_2D=1 (diagnosis): each record the 2D draws use, once: name, kind, TEX0's format and size, the GS-order flag.
            static const bool s_probe2D = [] { const char *e = std::getenv("DC_PROBE_2D"); return e && e[0] == '1'; }();
            static std::unordered_map<uint32_t, bool> s_seen2D;
            if (s_probe2D && record != 0u && !s_seen2D[record])
            {
                s_seen2D[record] = true;
                char name[33] = {};
                for (uint32_t k = 0; k < 32u; ++k)
                    name[k] = static_cast<char>(rdram[(record + 8u + k) & 0x1FFFFFFu]);
                const uint64_t tex0 = rd64(rdram, record + 0x28u);
                std::fprintf(stderr, "[dc:2d] record %08x \"%s\" kind %u psm 0x%x %ux%u tw %u th %u csa %u flag %u: %s\n", record, name,
                             rd32(rdram, record + 4u) >> 16, static_cast<unsigned>((tex0 >> 20) & 0x3Fu), rd32(rdram, record) >> 16,
                             rd32(rdram, record + 4u) & 0xFFFFu, static_cast<unsigned>((tex0 >> 26) & 0xFu), static_cast<unsigned>((tex0 >> 30) & 0xFu),
                             static_cast<unsigned>((tex0 >> 56) & 0x1Fu), rd32(rdram, record + 0x4Cu), texture ? "decoded" : "NOT decoded");
            }
            static std::unordered_map<uint32_t, int> s_draws2D;
            if (s_probe2D && record != 0u && s_draws2D[record]++ < 3)
                std::fprintf(stderr, "[dc:2d]   draw %08x: (%.1f %.1f) (%.1f %.1f) uv (%.0f %.0f)-(%.0f %.0f) rgba %08x alpha %llx\n", record,
                             c[0].x, c[0].y, c[3].x, c[3].y, c[0].u, c[0].v, c[3].u, c[3].v, c[0].rgba,
                             static_cast<unsigned long long>(g_alpha2D));
            if (record != 0u && !texture)
                return; // not decodable yet (counted with the GS-order textures not in the store)
            ps2x::native::Material material;
            material.textured = texture != nullptr;
            material.tex0 = record != 0u ? rd64(rdram, record + 0x28u) : 0u;
            material.blend = ps2x::native::blendOf(g_alpha2D);
            material.depthTest = false;
            material.depthWrite = false;
            material.clampU = material.clampV = true;
            const float tw = texture ? static_cast<float>(texture->width) : 1.0f, th = texture ? static_cast<float>(texture->height) : 1.0f;
            std::vector<ps2x::native::Vertex> vertices;
            auto vertex = [&](const Corner &k) {
                ps2x::native::Vertex v;
                v.x = k.x;
                v.y = k.y;
                v.u = k.u / tw;
                v.v = k.v / th;
                v.rgba = flat ? flatColour(k.rgba, k.rgba >> 8, k.rgba >> 16, k.rgba >> 24) : k.rgba;
                return v;
            };
            vertices.insert(vertices.end(), {vertex(c[0]), vertex(c[1]), vertex(c[2]), vertex(c[1]), vertex(c[3]), vertex(c[2])});
            drawScreen(material, vertices, texture);
        }
        struct Rect
        {
            int32_t x, y, w, h;
        };
        Rect rectAt(const uint8_t *rdram, uint32_t address)
        {
            return {static_cast<int32_t>(rd32(rdram, address)), static_cast<int32_t>(rd32(rdram, address + 4u)),
                    static_cast<int32_t>(rd32(rdram, address + 8u)), static_cast<int32_t>(rd32(rdram, address + 12u))};
        }
        // A sprite: destination d (DC's view), texels from (u0, v0) to (u1, v1), one colour; the right edge one 1/16 short
        // as the game writes it (0x6BFF).
        void drawSprite(const uint8_t *rdram, uint32_t record, const Rect &d, float u0, float v0, float u1, float v1, uint32_t rgba)
        {
            const float x0 = kOriginX + static_cast<float>(d.x), x1 = kOriginX + static_cast<float>(d.x + d.w) - 1.0f / 16.0f;
            const float y0 = kOriginY + static_cast<float>(d.y) / 2.0f, y1 = kOriginY + static_cast<float>(d.y + d.h) / 2.0f;
            const Corner c[4] = {{x0, y0, u0, v0, rgba}, {x1, y0, u1, v0, rgba}, {x0, y1, u0, v1, rgba}, {x1, y1, u1, v1, rgba}};
            drawStrip(rdram, record, c);
        }
        // set2DSprite (0x15BD00: a1 texture, a2 destination, a3 / t0 the source's corner, its size the destination's).
        void sprite2DHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t texture = getRegU32(ctx, 5);
            if (texture == 0u)
                return;
            const Rect d = rectAt(rdram, getRegU32(ctx, 6));
            const float u = static_cast<float>(static_cast<int32_t>(getRegU32(ctx, 7))), v = static_cast<float>(static_cast<int32_t>(getRegU32(ctx, 8)));
            drawSprite(rdram, texture, d, u, v, u + static_cast<float>(d.w), v + static_cast<float>(d.h), 0x80808080u);
        }
        // set2DSprite (0x15C000: a1 texture, a2 destination, a3 source), ...Uc (0x15C310: t0 the alpha), ...UcUcUcUc
        // (0x15C630: t0..t3 the colour).
        void sprite2DRectsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t entry = ctx->pc;
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t texture = getRegU32(ctx, 5);
            if (texture == 0u)
                return;
            const Rect d = rectAt(rdram, getRegU32(ctx, 6)), src = rectAt(rdram, getRegU32(ctx, 7));
            uint32_t rgba = 0x80808080u;
            if (entry == 0x0015C310u)
                rgba = 0x00808080u | (getRegU32(ctx, 8) & 0xFFu) << 24;
            else if (entry == 0x0015C630u)
                rgba = (getRegU32(ctx, 8) & 0xFFu) | (getRegU32(ctx, 9) & 0xFFu) << 8 | (getRegU32(ctx, 10) & 0xFFu) << 16 |
                       (getRegU32(ctx, 11) & 0xFFu) << 24;
            drawSprite(rdram, texture, d, static_cast<float>(src.x), static_cast<float>(src.y), static_cast<float>(src.x + src.w),
                       static_cast<float>(src.y + src.h), rgba);
        }
        // set2DSprite with four colours (0x15C980: a1 texture, a2 destination, a3 source, t0..t3 the corners' spRGBA
        // (top-left, top-right, bottom-left, bottom-right), the 9th argument the strip's order: 0 starts top-right).
        void sprite2DCornersHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t texture = getRegU32(ctx, 5);
            if (texture == 0u)
                return;
            const Rect d = rectAt(rdram, getRegU32(ctx, 6)), src = rectAt(rdram, getRegU32(ctx, 7));
            const uint32_t tl = rd32(rdram, getRegU32(ctx, 8)), tr = rd32(rdram, getRegU32(ctx, 9)), bl = rd32(rdram, getRegU32(ctx, 10)),
                           br = rd32(rdram, getRegU32(ctx, 11));
            const float x0 = kOriginX + static_cast<float>(d.x), x1 = kOriginX + static_cast<float>(d.x + d.w) - 1.0f / 16.0f;
            const float y0 = kOriginY + static_cast<float>(d.y) / 2.0f, y1 = kOriginY + static_cast<float>(d.y + d.h) / 2.0f;
            const float u0 = static_cast<float>(src.x), v0 = static_cast<float>(src.y), u1 = static_cast<float>(src.x + src.w),
                        v1 = static_cast<float>(src.y + src.h);
            if (stackArg(rdram, ctx, 0) == 0u)
            {
                const Corner c[4] = {{x1, y0, u1, v0, tr}, {x0, y0, u0, v0, tl}, {x1, y1, u1, v1, br}, {x0, y1, u0, v1, bl}};
                drawStrip(rdram, texture, c);
            }
            else
            {
                const Corner c[4] = {{x0, y0, u0, v0, tl}, {x1, y0, u1, v0, tr}, {x0, y1, u0, v1, bl}, {x1, y1, u1, v1, br}};
                drawStrip(rdram, texture, c);
            }
        }
        // set2DSpriteC4 (0x15E120: a1 the rectangle, a2..a5 the corners' spRGBA): an untextured gradient strip.
        void sprite2DC4Hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const Rect d = rectAt(rdram, getRegU32(ctx, 5));
            const float x0 = kOriginX + static_cast<float>(d.x), x1 = kOriginX + static_cast<float>(d.x + d.w) - 1.0f / 16.0f;
            const float y0 = kOriginY + static_cast<float>(d.y) / 2.0f, y1 = kOriginY + static_cast<float>(d.y + d.h) / 2.0f;
            const Corner c[4] = {{x0, y0, 0, 0, rd32(rdram, getRegU32(ctx, 6))}, {x1, y0, 0, 0, rd32(rdram, getRegU32(ctx, 7))},
                                 {x0, y1, 0, 0, rd32(rdram, getRegU32(ctx, 8))}, {x1, y1, 0, 0, rd32(rdram, getRegU32(ctx, 9))}};
            drawStrip(rdram, 0u, c, true);
        }
        // setColSprite (0x15DED0: a1..a4 four points, x, y in 1/16 GS pixels and z, t0..t3 the colour): an untextured
        // strip (PRIM 0x44).
        void colSpriteHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t rgba = (getRegU32(ctx, 8) & 0xFFu) | (getRegU32(ctx, 9) & 0xFFu) << 8 | (getRegU32(ctx, 10) & 0xFFu) << 16 |
                                  (getRegU32(ctx, 11) & 0xFFu) << 24;
            Corner c[4];
            for (uint32_t i = 0; i < 4u; ++i)
            {
                const uint32_t point = getRegU32(ctx, 5 + static_cast<int>(i));
                c[i] = {static_cast<float>(static_cast<int32_t>(rd32(rdram, point))) / 16.0f,
                        static_cast<float>(static_cast<int32_t>(rd32(rdram, point + 4u))) / 16.0f, 0.0f, 0.0f, rgba};
            }
            drawStrip(rdram, 0u, c, true);
        }
        // set2DSprite with a rotation (0x15E540: f12 the angle, a1 texture, a2 destination, a3 source, t0 / t1 the pivot in
        // the destination) and set2DSpriteRot with a colour (0x15EAD0: the same, t2 / t3 and the 9th / 10th arguments the
        // colour): the corners turned about the pivot with the game's own formulas.
        void sprite2DRotHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t entry = ctx->pc;
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t texture = getRegU32(ctx, 5);
            if (texture == 0u)
                return;
            const float angle = ctx->f[12];
            const Rect d = rectAt(rdram, getRegU32(ctx, 6)), src = rectAt(rdram, getRegU32(ctx, 7));
            const int32_t px = static_cast<int32_t>(getRegU32(ctx, 8)), py = static_cast<int32_t>(getRegU32(ctx, 9));
            const float c = std::cos(angle), sn = std::sin(angle);
            float xs[4], ys[4];
            uint32_t rgba = 0x80808080u;
            if (entry == 0x001831D0u)
            {
                // set2DSpriteRot ...Uc (0x1831D0: t2 the alpha): corners in 1/16 pixels, x = y' cos + x' sin, y halved.
                xs[0] = xs[2] = static_cast<float>(px * -16), xs[1] = xs[3] = static_cast<float>((d.w - px) * 16 - 1);
                ys[0] = ys[1] = static_cast<float>(py * -16), ys[2] = ys[3] = static_cast<float>((d.h - py) * 16 - 1);
                for (int i = 0; i < 4; ++i)
                {
                    const int32_t x = static_cast<int32_t>(ys[i] * c + xs[i] * sn);
                    const int32_t y = static_cast<int32_t>((xs[i] * c - ys[i] * sn) * 0.5f);
                    xs[i] = static_cast<float>(x + d.x * 16 + 0x6C00) / 16.0f;
                    ys[i] = static_cast<float>(y + d.y * 8 + 0x7900) / 16.0f;
                }
                rgba = 0x00808080u | (getRegU32(ctx, 10) & 0xFFu) << 24;
            }
            else if (entry == 0x0015E540u)
            {
                xs[0] = xs[2] = static_cast<float>(-px), xs[1] = xs[3] = static_cast<float>(d.w + 1 - px);
                ys[0] = ys[1] = static_cast<float>(-py), ys[2] = ys[3] = static_cast<float>(d.h + 1 - py);
                for (int i = 0; i < 4; ++i)
                {
                    const int32_t x = static_cast<int32_t>(ys[i] * c + xs[i] * sn), y = static_cast<int32_t>(xs[i] * c - ys[i] * sn);
                    xs[i] = static_cast<float>((x + d.x) * 16 + 0x6C00) / 16.0f;
                    ys[i] = static_cast<float>((y + d.y) * 8 + 0x7900) / 16.0f;
                }
            }
            else
            {
                xs[0] = xs[2] = static_cast<float>(px * -16), xs[1] = xs[3] = static_cast<float>((d.w - px) * 16 - 1);
                ys[0] = ys[1] = static_cast<float>(py * -16), ys[2] = ys[3] = static_cast<float>((d.h - py) * 16 - 1);
                for (int i = 0; i < 4; ++i)
                {
                    const int32_t x = static_cast<int32_t>(-ys[i] * sn - xs[i] * c);
                    const int32_t y = static_cast<int32_t>((-xs[i] * sn + ys[i] * c) * 0.5f);
                    xs[i] = static_cast<float>(x + d.x * 16 + 0x6C00) / 16.0f;
                    ys[i] = static_cast<float>(y + d.y * 8 + 0x7900) / 16.0f;
                }
                rgba = (getRegU32(ctx, 10) & 0xFFu) | (getRegU32(ctx, 11) & 0xFFu) << 8 | (stackArg(rdram, ctx, 0) & 0xFFu) << 16 |
                       (stackArg(rdram, ctx, 1) & 0xFFu) << 24;
            }
            const float u0 = static_cast<float>(src.x), v0 = static_cast<float>(src.y);
            const Corner k[4] = {{xs[0], ys[0], u0, v0, rgba},
                                 {xs[1], ys[1], static_cast<float>(src.x + d.w), v0, rgba},
                                 {xs[2], ys[2], u0, static_cast<float>(src.y + src.h), rgba},
                                 {xs[3], ys[3], static_cast<float>(src.x + src.w), static_cast<float>(src.y + src.h), rgba}};
            drawStrip(rdram, texture, k);
        }
        // Myset2DSprite (0x14CD80: a1 texture, a2 destination, a3 source, t0..t3 the colour) and set2DSprite_Core
        // (0x14C3C0: the same arguments; the sprite set2DSprite_Start opened a batch for, the message windows' glyphs).
        void mySpriteHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t texture = getRegU32(ctx, 5);
            if (texture == 0u)
                return;
            const Rect d = rectAt(rdram, getRegU32(ctx, 6)), src = rectAt(rdram, getRegU32(ctx, 7));
            const uint32_t rgba = (getRegU32(ctx, 8) & 0xFFu) | (getRegU32(ctx, 9) & 0xFFu) << 8 | (getRegU32(ctx, 10) & 0xFFu) << 16 |
                                  (getRegU32(ctx, 11) & 0xFFu) << 24;
            drawSprite(rdram, texture, d, static_cast<float>(src.x), static_cast<float>(src.y), static_cast<float>(src.x + src.w),
                       static_cast<float>(src.y + src.h), rgba);
        }
        // set2DSprite_Start / _End (0x14C220 / 0x14C4F0): the batch they open and close is not built; its sprites are
        // drawn one by one (set2DSprite_Core).
        void spriteBatchHook(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
        }
        // The texture record a TEX0 belongs to (sprite tables keep a TEX0, not the CTexture): the newest record with that
        // TBP, CBP and format, from the texture manager's own records.
        uint32_t recordOfTex0(const uint8_t *rdram, uint64_t tex0)
        {
            constexpr uint64_t kMask = 0x3FFFull | 0x3Full << 20 | 0x3FFFull << 37; // TBP0, PSM, CBP
            const int32_t count = static_cast<int32_t>(rd32(rdram, kTextureManager));
            for (int32_t i = std::min(count, 1023); i >= 0; --i)
            {
                const uint32_t record = kTextureManager + 0x10F8u + static_cast<uint32_t>(i) * 0x50u;
                if ((rd64(rdram, record + 0x28u) & kMask) == (tex0 & kMask) && rdram[(record + 8u) & 0x1FFFFFFu] != 0u)
                    return record;
            }
            return 0u;
        }
        // CSpriteTable::DrawTable (0x12BA20, a0 the table): the game's sprite lists (+ 4 the count, + 0xC the lists'
        // first entries, + 0x4C their ends). An entry: shorts x, y, w, h (DC's view), u, v, uw, vh, r, g, b, a, TEX0 at
        // + 0x28, the next entry at + 0x30. Sprites with mgAlpha's blend, Z test ALWAYS, no Z write.
        void spriteTableHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t table = getRegU32(ctx, 4);
            const int32_t lists = static_cast<int32_t>(rd32(rdram, table + 4u));
            for (int32_t list = lists - 1; list >= 0 && list < 16; --list)
            {
                uint32_t entry = rd32(rdram, table + 0xCu + static_cast<uint32_t>(list) * 4u);
                const uint32_t end = rd32(rdram, table + 0x4Cu + static_cast<uint32_t>(list) * 4u);
                for (int guard = 0; entry != end && entry != 0u && guard < 4096; ++guard)
                {
                    auto sh = [&](uint32_t i) { return static_cast<int32_t>(static_cast<int16_t>(rd32(rdram, entry + i * 2u) & 0xFFFFu)); };
                    const Rect d{sh(0), sh(1), sh(2), sh(3)};
                    const float u = static_cast<float>(sh(4)), v = static_cast<float>(sh(5));
                    const uint32_t rgba = static_cast<uint32_t>(sh(8) & 0xFF) | static_cast<uint32_t>(sh(9) & 0xFF) << 8 |
                                          static_cast<uint32_t>(sh(10) & 0xFF) << 16 | static_cast<uint32_t>(sh(11) & 0xFF) << 24;
                    const uint32_t record = recordOfTex0(rdram, rd64(rdram, entry + 0x28u));
                    if (record != 0u)
                    {
                        // The bottom edge one 1/16 short as the table writes it (0x78FF).
                        const float x0 = kOriginX + static_cast<float>(d.x), x1 = kOriginX + static_cast<float>(d.x + d.w) - 1.0f / 16.0f;
                        const float y0 = kOriginY + static_cast<float>(d.y) / 2.0f;
                        const float y1 = kOriginY + static_cast<float>(d.y + d.h) / 2.0f - 1.0f / 16.0f;
                        const float u1 = u + static_cast<float>(sh(6)), v1 = v + static_cast<float>(sh(7));
                        const Corner c[4] = {{x0, y0, u, v, rgba}, {x1, y0, u1, v, rgba}, {x0, y1, u, v1, rgba}, {x1, y1, u1, v1, rgba}};
                        drawStrip(rdram, record, c);
                    }
                    entry = rd32(rdram, entry + 0x30u);
                }
            }
        }

        // The effects' 3D sprites (set3DSprite / set3DSpriteFog / set3DColSprite, 0x15D1B0..): points the game projected
        // itself, in GS terms (x, y in 1/16 pixels, Z, and F for the fog ones), a source rectangle and colours. Strips or
        // sprites with PRIM ABE, the game's TEST (its depth test against the scene), Z not written.
        struct Point3
        {
            float x, y, z, fog;
        };
        Point3 pointAt(const uint8_t *rdram, uint32_t address, bool withFog)
        {
            Point3 p;
            p.x = static_cast<float>(static_cast<int32_t>(rd32(rdram, address))) / 16.0f;
            p.y = static_cast<float>(static_cast<int32_t>(rd32(rdram, address + 4u))) / 16.0f;
            p.z = static_cast<float>(rd32(rdram, address + 8u) & 0xFFFFFFu);
            p.fog = withFog ? static_cast<float>(rd32(rdram, address + 12u) & 0xFFu) / 255.0f : 1.0f;
            return p;
        }
        // A strip of four points (corner order TL, TR, BL, BR for the textured ones).
        void draw3D(const uint8_t *rdram, uint32_t record, const Point3 (&p)[4], const float (&u)[4], const float (&v)[4],
                    const uint32_t (&rgba)[4], bool fog, bool flat)
        {
            std::shared_ptr<const ps2x::native::TextureCache::Texture> texture = record != 0u ? recordTexture(rdram, record) : nullptr;
            if (record != 0u && !texture)
                return;
            ps2x::native::Material material;
            material.textured = texture != nullptr;
            material.tex0 = record != 0u ? rd64(rdram, record + 0x28u) : 0u;
            material.blend = ps2x::native::blendOf(g_alpha2D);
            material.depthTest = true;
            material.depthWrite = false;
            material.clampU = material.clampV = true;
            material.fog = fog;
            material.fogColour = g_fogColour;
            const float tw = texture ? static_cast<float>(texture->width) : 1.0f, th = texture ? static_cast<float>(texture->height) : 1.0f;
            ps2x::native::Vertex k[4];
            for (int i = 0; i < 4; ++i)
            {
                k[i].x = p[i].x;
                k[i].y = p[i].y;
                k[i].z = p[i].z;
                k[i].u = u[i] / tw;
                k[i].v = v[i] / th;
                k[i].fog = p[i].fog;
                k[i].rgba = flat ? flatColour(rgba[i], rgba[i] >> 8, rgba[i] >> 16, rgba[i] >> 24) : rgba[i];
            }
            const std::vector<ps2x::native::Vertex> vertices = {k[0], k[1], k[2], k[1], k[3], k[2]};
            drawScreen(material, vertices, texture);
        }
        // set3DColSprite (0x15D1B0: a1..a4 the points, t0..t3 their spRGBA): untextured, Gouraud.
        void colSprite3DHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            Point3 p[4];
            uint32_t rgba[4];
            for (int i = 0; i < 4; ++i)
            {
                p[i] = pointAt(rdram, getRegU32(ctx, 5 + i), false);
                rgba[i] = rd32(rdram, getRegU32(ctx, 8 + i));
            }
            const float zero[4] = {};
            draw3D(rdram, 0u, p, zero, zero, rgba, false, true);
        }
        // set3DSprite (0x15D4B0: a1 texture, a2 source, a3 / t0 / t1 / t2 the points TL, TR, BL, BR, t3 the spRGBA; 0x15D790:
        // a3 / t0 the sprite's corners, t1 the spRGBA) and set3DSpriteFog (0x15D9A0: the four points with F, t3 the alpha;
        // 0x15DCA0: the sprite's corners with F, t1 the spRGBA).
        void sprite3DHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t entry = ctx->pc;
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t texture = getRegU32(ctx, 5);
            if (texture == 0u)
                return;
            const Rect src = rectAt(rdram, getRegU32(ctx, 6));
            const bool fog = entry == 0x0015D9A0u || entry == 0x0015DCA0u;
            const bool strip = entry == 0x0015D4B0u || entry == 0x0015D9A0u;
            Point3 p[4];
            uint32_t colour = 0;
            if (strip)
            {
                for (int i = 0; i < 4; ++i)
                    p[i] = pointAt(rdram, getRegU32(ctx, 7 + i), fog);
                colour = entry == 0x0015D9A0u ? 0x00808080u | (getRegU32(ctx, 11) & 0xFFu) << 24 : rd32(rdram, getRegU32(ctx, 11));
            }
            else
            {
                const Point3 a = pointAt(rdram, getRegU32(ctx, 7), fog), b = pointAt(rdram, getRegU32(ctx, 8), fog);
                p[0] = a;
                p[1] = {b.x, a.y, b.z, b.fog};
                p[2] = {a.x, b.y, b.z, b.fog};
                p[3] = b;
                colour = rd32(rdram, getRegU32(ctx, 9));
            }
            const float u0 = static_cast<float>(src.x), u1 = static_cast<float>(src.x + src.w);
            const float v0 = static_cast<float>(src.y), v1 = static_cast<float>(src.y + src.h);
            const float u[4] = {u0, u1, u0, u1}, v[4] = {v0, v0, v1, v1};
            const uint32_t rgba[4] = {colour, colour, colour, colour};
            draw3D(rdram, texture, p, u, v, rgba, fog, false);
        }
        // DrawLine (0x171210: a0 / a1 the ends, x, y in 1/16 pixels and Z; a2..a5 the colour): a line, drawn as a thin quad.
        void drawLineHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const Point3 a = pointAt(rdram, getRegU32(ctx, 4), false), b = pointAt(rdram, getRegU32(ctx, 5), false);
            const uint32_t rgba = (getRegU32(ctx, 6) & 0xFFu) | (getRegU32(ctx, 7) & 0xFFu) << 8 | (getRegU32(ctx, 8) & 0xFFu) << 16 |
                                  (getRegU32(ctx, 9) & 0xFFu) << 24;
            float nx = -(b.y - a.y), ny = b.x - a.x;
            const float length = std::sqrt(nx * nx + ny * ny);
            if (length <= 0.0f)
                return;
            nx *= 0.5f / length, ny *= 0.5f / length;
            const Point3 p[4] = {{a.x - nx, a.y - ny, a.z, 1.0f}, {a.x + nx, a.y + ny, a.z, 1.0f}, {b.x - nx, b.y - ny, b.z, 1.0f},
                                 {b.x + nx, b.y + ny, b.z, 1.0f}};
            const float zero[4] = {};
            const uint32_t colours[4] = {rgba, rgba, rgba, rgba};
            draw3D(rdram, 0u, p, zero, zero, colours, false, true);
        }

        // MGMoveImage (0x12F210: a0 the source's TEX0, a1 the source rectangle in texels, a2 the destination's TEX0, a3 / t0
        // the destination point): a copy between two textures' places in GS memory (TexAnime's face expressions and
        // scrolling textures). Here it patches the destination record's decoded texture with the source record's
        // texels; the patched texture stands for the record until the game sends its own pixels again (its content
        // hash changes), as GS memory would keep the copy. Copies from or to a place no record holds (the frame: the
        // water's refraction copy) are not made.
        void patchTexture(const uint8_t *rdram, uint32_t src, const Rect &r, uint32_t dst, int32_t dx, int32_t dy);
        void moveImageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            constexpr uint64_t kPlace = 0x3FFFull | 0x3Full << 20; // TBP0, PSM
            const uint64_t srcTex0 = rd64(rdram, getRegU32(ctx, 4)), dstTex0 = rd64(rdram, getRegU32(ctx, 6));
            const Rect r = rectAt(rdram, getRegU32(ctx, 5));
            const int32_t dx = static_cast<int32_t>(getRegU32(ctx, 7)), dy = static_cast<int32_t>(getRegU32(ctx, 8));
            auto recordAt = [&](uint64_t tex0) {
                const int32_t count = static_cast<int32_t>(rd32(rdram, kTextureManager));
                for (int32_t i = std::min(count, 1023); i >= 0; --i)
                {
                    const uint32_t record = kTextureManager + 0x10F8u + static_cast<uint32_t>(i) * 0x50u;
                    if ((rd64(rdram, record + 0x28u) & kPlace) == (tex0 & kPlace) && rdram[(record + 8u) & 0x1FFFFFFu] != 0u)
                        return record;
                }
                return 0u;
            };
            const uint32_t src = recordAt(srcTex0), dst = recordAt(dstTex0);
            ++g_stats.textureCopies;
            if (src == 0u || dst == 0u)
            {
                ++g_stats.textureCopiesMissed;
                return;
            }
            patchTexture(rdram, src, r, dst, dx, dy);
        }
        // The copy itself: the destination record's decoded texture patched with the source record's texels.
        void patchTexture(const uint8_t *rdram, uint32_t src, const Rect &r, uint32_t dst, int32_t dx, int32_t dy)
        {
            if (r.w <= 0 || r.h <= 0)
                return;
            const auto from = recordTexture(rdram, src), to = recordTexture(rdram, dst);
            if (!from || !to)
                return;
            auto patched = std::make_shared<ps2x::native::TextureCache::Texture>(*to);
            for (int32_t y = 0; y < r.h; ++y)
                for (int32_t x = 0; x < r.w; ++x)
                {
                    const int32_t sx = r.x + x, sy = r.y + y, tx = dx + x, ty = dy + y;
                    if (sx < 0 || sy < 0 || tx < 0 || ty < 0 || sx >= static_cast<int32_t>(from->width) ||
                        sy >= static_cast<int32_t>(from->height) || tx >= static_cast<int32_t>(patched->width) ||
                        ty >= static_cast<int32_t>(patched->height))
                        continue;
                    patched->rgba[static_cast<size_t>(ty) * patched->width + static_cast<size_t>(tx)] =
                        from->rgba[static_cast<size_t>(sy) * from->width + static_cast<size_t>(sx)];
                }
            patched->version = ++g_textureVersion;
            auto &slot = g_decoded[dst];
            if (slot.second == to)
                slot.second = patched;
        }

        // The message balloons (ClsMes::MakeFukidashi 0x151AD0, a0 the ClsMes): the game points its frame at the balloon
        // texture (0x299A78), clears it and draws the balloon's shape there (MakeFukidashi_sub 0x150F50: a fan of 15
        // points from its table 0x253FA0, or DrawMaru's ellipses 0x150BB0, then the tail), grey 0xBF with alpha 0x80;
        // DrawMesWin then lays that texture on the screen. Here the shape goes to offscreen target 1, which the
        // balloon texture's sprites sample. Its patterned fill (a texture drawn under the destination alpha test) is
        // not drawn yet.
        void balloonFan(std::vector<ps2x::native::Vertex> &out, const std::vector<std::pair<float, float>> &points)
        {
            for (size_t i = 1; i + 1 < points.size(); ++i)
                for (const auto &[x, y] : {points[0], points[i], points[i + 1]})
                {
                    ps2x::native::Vertex v;
                    v.x = kOriginX + x;
                    v.y = kOriginY + y;
                    v.rgba = flatColour(0xBF, 0xBF, 0xBF, 0x80);
                    out.push_back(v);
                }
        }
        // DrawMaru (x, y, w, h, the vertical offset, the wobble mode): an ellipse as a fan from its centre.
        void balloonEllipse(const uint8_t *rdram, std::vector<ps2x::native::Vertex> &out, int32_t x, int32_t y, int32_t w, int32_t h,
                            int32_t offset, bool wobble)
        {
            const float turn = rdf(rdram, 0x002A1988u), wobbleX = rdf(rdram, 0x002A1874u), wobbleY = rdf(rdram, 0x002A1960u);
            std::vector<std::pair<float, float>> points;
            points.emplace_back(static_cast<float>(x + (w >> 1)), static_cast<float>(y + (h >> 1) - offset));
            uint32_t i = 0;
            for (float f = 0.0f, step = 0.0f; f < 128.0f && i < 512u; f += step, ++i)
            {
                const float angle = turn * f / 128.0f;
                float c = std::cos(angle), sn = std::sin(angle);
                if (wobble && (i & 1u))
                    c *= wobbleX, sn *= wobbleY;
                points.emplace_back(static_cast<float>(static_cast<int32_t>((c + 1.0f) * static_cast<float>(w) / 2.0f) + x),
                                    static_cast<float>(static_cast<int32_t>((sn + 1.0f) * static_cast<float>(h) / 2.0f) + y - offset));
                step = wobble ? rdf(rdram, 0x01CF8FF0u + i * 4u) : 4.0f;
                if (!(step > 0.0f))
                    break;
            }
            float c = 1.0f, sn = 0.0f;
            if (wobble && (i & 1u))
                c *= wobbleX, sn *= wobbleY;
            points.emplace_back(static_cast<float>(static_cast<int32_t>((c + 1.0f) * static_cast<float>(w) / 2.0f) + x),
                                static_cast<float>(static_cast<int32_t>((sn + 1.0f) * static_cast<float>(h) / 2.0f) + y - offset));
            balloonFan(out, points);
        }
        void balloonHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t cls = getRegU32(ctx, 4);
            g_balloonRecord = textureRecord(rdram, 0x00299A78u);
            auto i32 = [&](uint32_t offset) { return static_cast<int32_t>(rd32(rdram, cls + offset)); };
            const float t = rdf(rdram, cls + 0x90u);
            auto lerp = [&](uint32_t a, uint32_t b) { return static_cast<int32_t>(static_cast<float>(i32(a)) + (static_cast<float>(i32(b)) - static_cast<float>(i32(a))) * t); };
            // The target cleared.
            ps2x::native::Material clear;
            clear.textured = false;
            clear.blend = ps2x::native::Material::Blend::Opaque;
            clear.depthTest = false;
            clear.depthWrite = false;
            clear.target = 1;
            std::vector<ps2x::native::Vertex> vertices;
            quad(vertices, kOriginX, kOriginY, kOriginX + 640.0f, kOriginY + 224.0f, 0.0f, 0u);
            drawScreen(clear, vertices);
            vertices.clear();
            // MakeFukidashi_sub: the shape.
            int32_t top = std::min(i32(0x48), i32(0x40));
            if (i32(0x58) != 0)
            {
                top = std::min({top, i32(0x78), i32(0x80), i32(0x88)});
                if (i32(0x38) == 2)
                    top = std::min(top, i32(0x88) - 5);
            }
            const float fw = static_cast<float>(i32(0x4C)) * t, fh = static_cast<float>(i32(0x50)) * t;
            float table[30];
            for (uint32_t k = 0; k < 30u; ++k)
                table[k] = rdf(rdram, 0x00253FA0u + k * 4u);
            const int32_t type = i32(0x38);
            const int32_t x0 = lerp(0x3C, 0x44), y0 = lerp(0x40, 0x48);
            if (type == 0)
            {
                const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(rdf(rdram, 0x01CF8FF0u)));
                std::vector<std::pair<float, float>> points;
                for (uint32_t k = 0; k < 15u; ++k)
                {
                    const int32_t px = static_cast<int32_t>(fw * ((u & 1u) == 0u ? table[k * 2] : 1.0f - table[k * 2]));
                    const int32_t py = static_cast<int32_t>(fh * (static_cast<int32_t>(u) < 2 ? table[k * 2 + 1] : 1.0f - table[k * 2 + 1]));
                    points.emplace_back(static_cast<float>(px + x0), static_cast<float>(py + y0 - top));
                }
                balloonFan(vertices, points);
            }
            else if (type == 1)
                balloonEllipse(rdram, vertices, x0, y0, static_cast<int32_t>(fw), static_cast<int32_t>(fh), top, true);
            else if (type == 2)
            {
                const int32_t half = static_cast<int32_t>(fh) >> 1;
                const float lift = static_cast<float>(i32(0x0C)) * 1.5f;
                balloonEllipse(rdram, vertices, x0, y0, static_cast<int32_t>(fw) - i32(8), static_cast<int32_t>(static_cast<float>(half) + lift), top, false);
                balloonEllipse(rdram, vertices, x0 + i32(8), static_cast<int32_t>(static_cast<float>(y0 + half) - lift),
                               static_cast<int32_t>(fw) - i32(8), static_cast<int32_t>(static_cast<float>(half) + lift), top, false);
            }
            else
            {
                const int32_t w = static_cast<int32_t>(fw), h = static_cast<int32_t>(fh);
                const float turn = rdf(rdram, 0x002A1988u);
                std::vector<std::pair<float, float>> points;
                points.emplace_back(static_cast<float>(x0 + (w >> 1)), static_cast<float>(y0 + (h >> 1) - top));
                float step = table[28];
                uint32_t k = 0;
                for (float f = 0.0f; f < 128.0f && k < 512u; f += step, ++k)
                {
                    const float angle = turn * f / 128.0f, r = rdf(rdram, 0x01CF90F0u + k * 4u);
                    points.emplace_back(static_cast<float>(static_cast<int32_t>((std::cos(angle) * r + 0.5f) * static_cast<float>(w)) + x0),
                                        static_cast<float>(static_cast<int32_t>((std::sin(angle) * r + 0.5f) * static_cast<float>(h)) + y0 - top));
                    step = rdf(rdram, 0x01CF8FF0u + k * 4u);
                    if (!(step > 0.0f))
                        break;
                }
                const float r0 = rdf(rdram, 0x01CF90F0u);
                points.emplace_back(static_cast<float>(static_cast<int32_t>((r0 + 0.5f) * static_cast<float>(w)) + x0),
                                    static_cast<float>(static_cast<int32_t>(0.5f * static_cast<float>(h)) + y0 - top));
                balloonFan(vertices, points);
            }
            if (i32(0x58) != 0)
            {
                if (type == 2)
                {
                    const int32_t cx = static_cast<int32_t>(static_cast<float>(i32(0x3C)) + (static_cast<float>((i32(0x64) + i32(0x84)) >> 1) - static_cast<float>(i32(0x3C))) * t);
                    const int32_t cy = static_cast<int32_t>(static_cast<float>(i32(0x40)) + (static_cast<float>((i32(0x68) + i32(0x88)) >> 1) - static_cast<float>(i32(0x40))) * t);
                    balloonEllipse(rdram, vertices, cx - 10, cy - 10, 0x14, 0x14, top, false);
                    balloonEllipse(rdram, vertices, lerp(0x3C, 0x84) - 5, lerp(0x40, 0x88) - 5, 10, 10, top, false);
                }
                else
                {
                    const std::vector<std::pair<float, float>> tail = {
                        {static_cast<float>(lerp(0x3C, 0x84)), static_cast<float>(lerp(0x40, 0x88) - top)},
                        {static_cast<float>(lerp(0x3C, 0x74)), static_cast<float>(lerp(0x40, 0x78) - top)},
                        {static_cast<float>(lerp(0x3C, 0x7C)), static_cast<float>(lerp(0x40, 0x80) - top)}};
                    balloonFan(vertices, tail);
                }
            }
            ps2x::native::Material shape;
            shape.textured = false;
            shape.blend = ps2x::native::Material::Blend::Alpha;
            shape.depthTest = false;
            shape.depthWrite = false;
            shape.target = 1;
            drawScreen(shape, vertices);
        }

        // MoveImageTest (0x140660: a1 / a2 / a3 the source's base, width and format, t0 the rectangle, t1 / t2 / t3 the
        // destination's, the 9th / 10th arguments the destination point): the same copy as MGMoveImage between two
        // records' places, given by their GS base (the message windows build their text from the font this way).
        void moveImageTestHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            auto recordAt = [&](uint32_t base, uint32_t psm) {
                const int32_t count = static_cast<int32_t>(rd32(rdram, kTextureManager));
                for (int32_t i = std::min(count, 1023); i >= 0; --i)
                {
                    const uint32_t record = kTextureManager + 0x10F8u + static_cast<uint32_t>(i) * 0x50u;
                    const uint64_t tex0 = rd64(rdram, record + 0x28u);
                    if ((tex0 & 0x3FFFu) == base && ((tex0 >> 20) & 0x3Fu) == psm && rdram[(record + 8u) & 0x1FFFFFFu] != 0u)
                        return record;
                }
                return 0u;
            };
            const uint32_t src = recordAt(getRegU32(ctx, 5) & 0x3FFFu, getRegU32(ctx, 7) & 0x3Fu);
            const uint32_t dst = recordAt(getRegU32(ctx, 9) & 0x3FFFu, getRegU32(ctx, 11) & 0x3Fu);
            ++g_stats.textureCopies;
            if (src == 0u || dst == 0u)
            {
                ++g_stats.textureCopiesMissed;
                return;
            }
            patchTexture(rdram, src, rectAt(rdram, getRegU32(ctx, 8)), dst, static_cast<int32_t>(stackArg(rdram, ctx, 0)),
                         static_cast<int32_t>(stackArg(rdram, ctx, 1)));
        }
        // CFireOmni::DrawFire (0x161AC0): its sprites (set3DSpriteFog) drawn with the ALPHA it writes, 0x48: Cs As + Cd.
        PS2Runtime::RecompiledFunction g_drawFire = nullptr;
        void drawFireHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t saved = g_alpha2D;
            g_alpha2D = (g_alpha2D & ~0xFFull) | 0x48u;
            g_drawFire(rdram, ctx, runtime);
            g_alpha2D = saved;
        }

        // SetClut (0x14C5A0: a1 the texture whose CBP takes the palette, a2 the palette).
        void setClutHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = getRegU32(ctx, 31);
            const uint32_t texture = getRegU32(ctx, 5);
            if (texture != 0u)
                g_clutOverride[static_cast<uint32_t>((rd64(rdram, texture + 0x28u) >> 37) & 0x3FFFu)] = getRegU32(ctx, 6);
        }

        // The ALPHA the game sets for its 2D (setAlphaFlag 0x15BC20: a1 its sceGsAlpha; MGSetGsALPHA 0x12EF90: a0).
        PS2Runtime::RecompiledFunction g_setAlphaFlag = nullptr, g_setGsAlpha = nullptr;
        void setAlphaFlagHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_alpha2D = rd64(rdram, getRegU32(ctx, 5));
            g_setAlphaFlag(rdram, ctx, runtime);
        }
        void setGsAlphaHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // A null argument restores the game's default, mgAlpha (gp - 0x7408), as MGSetGsALPHA does.
            const uint32_t alpha = getRegU32(ctx, 4);
            g_alpha2D = rd64(rdram, alpha != 0u ? alpha : getRegU32(ctx, 28) - 0x7408u);
            g_setGsAlpha(rdram, ctx, runtime);
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
        // DC_SKIP_FN=<hex>[,<hex>...] (diagnosis): those routines (void draws) do nothing.
        if (const char *skip = std::getenv("DC_SKIP_FN"))
            for (const char *at = skip; *at != '\0';)
            {
                char *end = nullptr;
                const uint32_t address = static_cast<uint32_t>(std::strtoul(at, &end, 16));
                if (end == at)
                    break;
                if (address != 0u)
                    runtime.registerFunction(address, [](uint8_t *, R5900Context *ctx, PS2Runtime *) {
                        ctx->pc = getRegU32(ctx, 31);
                    });
                at = *end == ',' ? end + 1 : end;
            }
        if (const char *probe = std::getenv("DC_PROBE_TEX"); probe && probe[0] == '1')
            for (SpriteHook &hook : g_spriteHooks)
                if ((hook.original = runtime.lookupFunction(hook.address)))
                    runtime.registerFunction(hook.address, &spriteProbe);
        if (const char *probe = std::getenv("DC_PROBE_FEEDERS"); probe && probe[0] == '1')
            for (FeederHook &hook : g_feeders)
                if ((hook.original = runtime.lookupFunction(hook.address)))
                    runtime.registerFunction(hook.address, &feederHook);
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
        runtime.registerFunction(kClothDrawVu1, &clothDrawHook);
        runtime.registerFunction(kShadowDrawVu1, &shadowDrawHook);
        runtime.registerFunction(kWaterCreateVUData, &waterCreateHook);
        if (!(std::getenv("DC_NO_DOF") && std::getenv("DC_NO_DOF")[0] == '1'))
            runtime.registerFunction(0x001652C0u, &depthOfFieldHook);
        runtime.registerFunction(0x001308A0u, &beginShadowHook);
        runtime.registerFunction(0x00130B30u, &endShadowHook);
        ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
        renderer.setEnabled(true);
        renderer.beginFrame(screen());
        ps2x::native::setVu1Gate(&gate);
        // PS2X_GS_RENDERER=none: no GS, the frame is the native renderer's (present() draws it in the game's call order).
        // Otherwise (comparison path), in the GS stream where VU1's output would have been, sorted with the GS draws.
        const char *gsRenderer = std::getenv("PS2X_GS_RENDERER");
        g_noGs = gsRenderer && std::strcmp(gsRenderer, "none") == 0;
        renderer.setStreamDraws(!g_noGs);
        // No packet path either: the game's VIF1 and GIF transfers are not read (runtime/gs/gs_off.h); every VU1
        // program and every 2D draw is the adapter's, from the game's data.
        ps2x::setPacketPathOff(g_noGs);
        if (g_noGs)
        {
            runtime.registerFunction(0x00130000u, &clearScreenHook);
            runtime.registerFunction(0x0012FD30u, &clearZHook);
            runtime.registerFunction(0x0012FA70u, &fillBoxHook);
            runtime.registerFunction(0x0015BD00u, &sprite2DHook);
            for (uint32_t address : {0x0015C000u, 0x0015C310u, 0x0015C630u})
                runtime.registerFunction(address, &sprite2DRectsHook);
            runtime.registerFunction(0x0015C980u, &sprite2DCornersHook);
            runtime.registerFunction(0x0015E120u, &sprite2DC4Hook);
            runtime.registerFunction(0x0015DED0u, &colSpriteHook);
            for (uint32_t address : {0x0015E540u, 0x0015EAD0u, 0x001831D0u})
                runtime.registerFunction(address, &sprite2DRotHook);
            for (uint32_t address : {0x0014CD80u, 0x0014C3C0u})
                runtime.registerFunction(address, &mySpriteHook);
            for (uint32_t address : {0x0014C220u, 0x0014C4F0u})
                runtime.registerFunction(address, &spriteBatchHook);
            runtime.registerFunction(0x0014C5A0u, &setClutHook);
            runtime.registerFunction(0x0012BA20u, &spriteTableHook);
            runtime.registerFunction(0x0015D1B0u, &colSprite3DHook);
            for (uint32_t address : {0x0015D4B0u, 0x0015D790u, 0x0015D9A0u, 0x0015DCA0u})
                runtime.registerFunction(address, &sprite3DHook);
            runtime.registerFunction(0x00171210u, &drawLineHook);
            runtime.registerFunction(0x0012F210u, &moveImageHook);
            runtime.registerFunction(0x00151AD0u, &balloonHook);
            runtime.registerFunction(0x00140660u, &moveImageTestHook);
            if ((g_drawFire = runtime.lookupFunction(0x00161AC0u)))
                runtime.registerFunction(0x00161AC0u, &drawFireHook);
            if ((g_setAlphaFlag = runtime.lookupFunction(0x0015BC20u)) && (g_setGsAlpha = runtime.lookupFunction(0x0012EF90u)))
            {
                runtime.registerFunction(0x0015BC20u, &setAlphaFlagHook);
                runtime.registerFunction(0x0012EF90u, &setGsAlphaHook);
            }
        }
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
            std::fprintf(stderr, "[dc:native] per 300 frames: %llu DrawVu1 calls (%llu without a known MDT, %llu drawn from a copy of their MDT), %llu primitives, "
                                 "%llu triangles, %llu own textures, %llu untextured for lack of one; not drawn: %llu shadows, "
                                 "%llu screen-space; %llu cloths; %llu shadow triangles; %llu waters;%llu GS-order textures not in the store; VU1 programs skipped: %llu\n",
                         static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.noModel),
                         static_cast<unsigned long long>(g_stats.copiedModel),
                         static_cast<unsigned long long>(g_stats.primitives),
                         static_cast<unsigned long long>(g_stats.triangles),
                         static_cast<unsigned long long>(g_stats.ownTextures),
                         static_cast<unsigned long long>(g_stats.noTexture),
                         static_cast<unsigned long long>(g_stats.shadows),
                         static_cast<unsigned long long>(g_stats.screenSpace),
                         static_cast<unsigned long long>(g_stats.cloths),
                         static_cast<unsigned long long>(g_stats.shadowTriangles),
                         static_cast<unsigned long long>(g_stats.waters),
                         static_cast<unsigned long long>(g_stats.gsOrderTextures),
                         static_cast<unsigned long long>(g_gateSkipped.exchange(0)));
            std::fprintf(stderr, "[dc:native] groups: %llu numbered here; skipped at pc 0: %llu, pc 8: %llu, other: %llu (last 0x%llx)\n",
                         static_cast<unsigned long long>(g_group), static_cast<unsigned long long>(g_gateByPc[0].exchange(0)),
                         static_cast<unsigned long long>(g_gateByPc[1].exchange(0)),
                         static_cast<unsigned long long>(g_gateByPc[2].exchange(0)),
                         static_cast<unsigned long long>(g_gateByPc[3].load()));
            std::fprintf(stderr, "[dc:native] objects without Z write (ZBUF ZMSK): %llu; depths of field: %llu; back faces culled: %llu; outside the guard band: %llu\n",
                         static_cast<unsigned long long>(g_stats.noZWrite), static_cast<unsigned long long>(g_stats.depthOfFields),
                         static_cast<unsigned long long>(g_stats.culled), static_cast<unsigned long long>(g_stats.guarded));
            if (g_stats.textureCopies != 0u)
                std::fprintf(stderr, "[dc:native] texture copies: %llu, %llu between places no record holds\n",
                             static_cast<unsigned long long>(g_stats.textureCopies), static_cast<unsigned long long>(g_stats.textureCopiesMissed));
            if (g_stats.maxZ > -1e30)
                std::fprintf(stderr, "[dc:native] GS Z of clipped vertices: %.0f .. %.0f, %llu past 2^24, %llu negative\n", g_stats.minZ,
                             g_stats.maxZ, static_cast<unsigned long long>(g_stats.zPast24), static_cast<unsigned long long>(g_stats.zNegative));
            g_stats = Stats{};
            for (FeederHook &hook : g_feeders)
                if (const uint64_t calls = hook.calls.exchange(0))
                    std::fprintf(stderr, "[dc:feeder] %s: %llu calls\n", hook.name, static_cast<unsigned long long>(calls));
            for (CountHook &hook : g_countHooks)
                if (const uint64_t calls = hook.calls.exchange(0))
                    std::fprintf(stderr, "[dc:probe] %s: %llu calls\n", hook.name, static_cast<unsigned long long>(calls));
        }
        renderer.endFrame();
        renderer.beginFrame(screen());
        g_clutOverride.clear();
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
