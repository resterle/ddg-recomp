// Vertex.cpp - CVertex (3D models) on top of the SDL3 GPU CGraphic
//
// Behaviour follows the original (see Vertex.h for addresses); the implementation is new.

#include "Vertex.h"
#include <cstdio>
#include <SDL3/SDL.h>
#include <cstdlib>
#include "Gs.h"
#include <cmath>
#include <cstring>

// getenv once per call site (debug hooks run in hot paths)
#define CACHED_ENV(name) ([]() -> const char * { static const char *v_ = getenv(name); return v_; }())

namespace {

constexpr uint32_t kRestart = 0x8000;       // position w marking vertices that start a strip

inline uint32_t U32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline float F32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
inline int16_t S16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }

// Words of payload for an UNPACK code (WL = CL, as in model data).
inline uint32_t UnpackWords(uint32_t code)
{
    uint32_t cmd = code >> 24, vn = (cmd >> 2) & 3, vl = cmd & 3, num = (code >> 16) & 0xff;
    return ((32u >> vl) * (vn + 1) * num + 31) / 32;
}

inline bool IsUnpack(uint32_t code) { return ((code >> 24) & 0x60) == 0x60; }

void LoadMatrix(float m[4][4], const uint8_t *p)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m[r][c] = F32(p + (r * 4 + c) * 4);
}

// Row-vector product (D3DXMatrixMultiply(out, a, b) = a * b).
void Multiply(float out[4][4], const float a[4][4], const float b[4][4])
{
    float t[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
    memcpy(out, t, sizeof(t));
}

} // namespace

bool CVertex::Init()
{
    m_Models.clear();
    m_Const = MeshConstants();
    return true;
}

void CVertex::Term()
{
    for (Model &m : m_Models) {
        if (!m.used) continue;
        CGraphic::DestroyMesh(m.pMesh);
        if (m.pHeader) memset(m.pHeader + 0x0c, 0, 4);
    }
    m_Models.clear();
}

uint8_t *CVertex::PacketBlock(uint8_t *pModel)
{
    uint32_t table = ((pModel[1] * 6 + 0x11) >> 4) * 0x10;     // texture table, 6 bytes per entry
    return pModel + table + ((pModel[0] & 1) ? 0 : 0x80);
}

uint32_t CVertex::PacketBlockSize(uint8_t *pModel)
{
    return (U32(PacketBlock(pModel)) & ~3u) + 0x10;
}

// ---------------------------------------------------------------------------------------------
// Registration (load time)

bool CVertex::RegisterObject(uint8_t *pObject, TXMHEADER *const *ppTexList, int nTexList, CTexture *pTexture)
{
    uint32_t flags = U32(pObject);
    if (flags & 0x80000000u) return true;                       // already registered
    for (int i = 0; i < 3; i++) {                               // three (offset, size) pairs at +0x10
        uint32_t off = U32(pObject + 0x10 + 8 * i), size = U32(pObject + 0x14 + 8 * i);
        if (size && !RegisterModel(pObject + off, ppTexList, nTexList, pTexture)) return false;
    }
    flags |= 0x80000000u;
    memcpy(pObject, &flags, 4);
    return true;
}

void CVertex::UnregisterObject(uint8_t *pObject)
{
    uint32_t flags = U32(pObject);
    if (!(flags & 0x80000000u)) return;
    flags &= 0x7fffffffu;
    memcpy(pObject, &flags, 4);
    for (int i = 0; i < 3; i++) {
        uint32_t off = U32(pObject + 0x10 + 8 * i), size = U32(pObject + 0x14 + 8 * i);
        if (size) UnregisterModel(pObject + off);
    }
}

bool CVertex::RegisterModel(uint8_t *pModel, TXMHEADER *const *ppTexList, int nTexList, CTexture *pTexture)
{
    // Texture table: (texture list index, image handle, CLUT handle) per entry, filled in here.
    uint8_t *table = pModel + 2;
    if (ppTexList) {
        for (int i = 0; i < pModel[1]; i++) {
            uint8_t *e = table + 6 * i;
            uint16_t index;
            memcpy(&index, e, 2);
            if (index >= nTexList) return false;
            TXMHEADER *rec = ppTexList[index];
            if (rec->state != 0x0f && !pTexture->Load(rec, false, 1)) return false;
            memcpy(e + 2, &rec->handle, 2);
            if (rec->clutState == 0x0f) memcpy(e + 4, &rec->clutHandle, 2);
        }
    }

    uint8_t *block = PacketBlock(pModel);
    uint8_t *end = block + PacketBlockSize(pModel);
    // Each packet: a qword whose last word is the header UNPACK, then three UNPACK payloads.
    for (uint8_t *q = block + 0x10; q + 0x10 < end;) {
        uint32_t c1 = U32(q + 0x0c);
        if (!IsUnpack(c1)) return false;
        uint8_t *d1 = q + 0x10;
        uint8_t *d2 = d1 + UnpackWords(c1) * 4 + 4;
        uint8_t *d3 = d2 + UnpackWords(U32(d2 - 4)) * 4 + 4;
        uint8_t *e3 = d3 + UnpackWords(U32(d3 - 4)) * 4;
        if (e3 > end) return false;
        if (!BuildPacket(d1, d2, d3)) return false;

        if (ppTexList) {
            // Patch TEX0 with the loaded handles: TBP0 = image, CBP = CLUT, PSM/CPSM = states.
            const uint8_t *e = table + 6 * (U32(d1 + 8) & 0xff);
            uint16_t index;
            memcpy(&index, e, 2);
            const TXMHEADER *rec = ppTexList[index];
            uint64_t tex0;
            memcpy(&tex0, d1 + 0x20, 8);
            tex0 = (tex0 & ~0x3fffull) | ((uint64_t)rec->handle & 0x3fff);
            tex0 = (tex0 & ~(0x3full << 20)) | ((uint64_t)(rec->state & 0x3f) << 20);
            tex0 = (tex0 & ~(0x3fffull << 37)) | ((uint64_t)rec->clutHandle & 0x3fff) << 37;
            tex0 = (tex0 & ~(0xfull << 51)) | ((uint64_t)(rec->clutState & 0x0f) << 51);
            memcpy(d1 + 0x20, &tex0, 8);
        }
        q = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(e3) + 15) & ~uintptr_t(15));
    }
    return true;
}

void CVertex::UnregisterModel(uint8_t *pModel)
{
    uint8_t *block = PacketBlock(pModel);
    uint8_t *end = block + PacketBlockSize(pModel);
    for (uint8_t *q = block + 0x10; q + 0x10 < end;) {
        uint32_t c1 = U32(q + 0x0c);
        if (!IsUnpack(c1)) return;
        uint8_t *d1 = q + 0x10;
        uint32_t handle = U32(d1 + 0x0c);
        if (handle && handle <= m_Models.size()) {
            Model &m = m_Models[handle - 1];
            CGraphic::DestroyMesh(m.pMesh);
            m = Model();
            memset(d1 + 0x0c, 0, 4);
        }
        uint8_t *d2 = d1 + UnpackWords(c1) * 4 + 4;
        uint8_t *d3 = d2 + UnpackWords(U32(d2 - 4)) * 4 + 4;
        uint8_t *e3 = d3 + UnpackWords(U32(d3 - 4)) * 4;
        q = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(e3) + 15) & ~uintptr_t(15));
    }
}

bool CVertex::BuildPacket(uint8_t *hdr, const uint8_t *pNormals, const uint8_t *pUVs)
{
    uint32_t flags = U32(hdr + 4);
    uint32_t type = (flags >> 9) & 7;
    bool normals = !((flags >> 12) & 1);
    uint32_t n = U32(hdr + 0x40) & 0x7fff;              // NLOOP of the header's GIF tag
    const uint8_t *pos = hdr + 0x50;
    if (type != 3 && type != 4) return true;            // the original only built these two

    // Fixed-point streams: normals and UVs are int16 / 4096. Without normals the original kept
    // reading the first normal for every vertex.
    auto vertex = [&](uint32_t i) {
        const uint8_t *p = pos + i * 16, *nm = pNormals + (normals ? i * 6 : 0), *uv = pUVs + i * 4;
        const float k = 1.0f / 4096.0f;
        return MeshVertex{F32(p), F32(p + 4), F32(p + 8), S16(nm) * k, S16(nm + 2) * k, S16(nm + 4) * k,
                          S16(uv) * k, S16(uv + 2) * k};
    };
    auto w = [&](uint32_t i) { return i < n ? U32(pos + i * 16 + 12) : 0u; };

    Model m;
    m.type = type;
    m.pHeader = hdr;
    std::vector<MeshVertex> v;
    if (type == 4) {
        // Strips: vertices flagged with w = 0x8000 begin a new strip. The original drew each strip
        // separately with the cull mode of its start parity (CW for even, CCW for odd starts) so
        // alternating strips keep a consistent facing. Here all strips of the model become one
        // triangle list culled CW, so the model is a single draw: odd triangles of a strip are
        // emitted with the order a strip implies, and triangles of CCW-culled strips are reversed
        // (culling CCW == reversing the winding and culling CW).
        std::vector<MeshVertex> strip;
        for (uint32_t i = 0; i < n;) {
            if (w(i + 2) == kRestart) i++;
            bool reverse = (i & 1) != 0;
            uint32_t len = 0;
            strip.clear();
            for (; i < n && (w(i) != kRestart || len < 3); i++, len++) strip.push_back(vertex(i));
            for (uint32_t t = 0; t + 2 < len; t++) {
                uint32_t a = t, b = t + 1, c = t + 2;
                if (t & 1) std::swap(a, b);             // strip winding alternates
                if (reverse) std::swap(a, b);
                v.push_back(strip[a]); v.push_back(strip[b]); v.push_back(strip[c]);
            }
        }
        m.type = 3;
        m.listPrims = (uint32_t)(v.size() / 3);
        m.listCull = d3d::CULL_CW;
    } else {
        // Lists: within every 6 vertices, the 5th and 6th swap places (re-winding the second
        // triangle).
        v.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = i % 6, dst = i - k + (k == 4 ? 5 : k == 5 ? 4 : k);
            if (dst < n) v[dst] = vertex(i);
        }
        m.listPrims = n / 3;
        m.listCull = d3d::CULL_CW;
    }
    if (v.empty()) return true;
    m.pMesh = CGraphic::CreateMesh(v.data(), (uint32_t)v.size());
    if (!m.pMesh) return false;
    m.used = true;
    m_Models.push_back(m);
    uint32_t handle = (uint32_t)m_Models.size();
    memcpy(hdr + 0x0c, &handle, 4);
    return true;
}

// ---------------------------------------------------------------------------------------------
// VU1 emulation (draw time)

void CVertex::Camera(const uint8_t *data)
{
    m_pCamera = data;
    // c1: fog start, end and 1 / (end - start)
    float start = F32(data + 0x6c), end = F32(data + 0x7c);
    m_Const.fog[0] = start;
    m_Const.fog[1] = end;
    m_Const.fog[2] = 1.0f / (end - start);
    m_Const.fog[3] = 0.0f;
    // World to screen = matrix at +0x80 times matrix at +0x180 (the original wrote it back to +0xC0).
    float a[4][4], b[4][4];
    LoadMatrix(a, data + 0x80);
    LoadMatrix(b, data + 0x180);
    Multiply(m_WorldToScreen, a, b);
    // c2-c4: light colour matrix at +0x140, transposed.
    float lc[4][4];
    LoadMatrix(lc, data + 0x140);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) m_Const.lightColor[r][c] = lc[c][r];
}

void CVertex::Object(const uint8_t *data)
{
    m_pObject = data;
    float model[4][4];
    LoadMatrix(model, data);
    Multiply(m_Const.localToScreen, model, m_WorldToScreen);       // c5-c8

    // c9-c11: light directions in model space. Each normalised model axis is run through the
    // camera's light-direction matrix (+0x100); transposing gives one row per light.
    if (m_pCamera) {
        float dir[4][4], lights[3][4];
        LoadMatrix(dir, m_pCamera + 0x100);
        for (int axis = 0; axis < 3; axis++) {
            float x = model[axis][0], y = model[axis][1], z = model[axis][2];
            float len = std::sqrt(x * x + y * y + z * z);
            if (len > 0) { x /= len; y /= len; z /= len; }
            for (int j = 0; j < 4; j++) lights[axis][j] = x * dir[0][j] + y * dir[1][j] + z * dir[2][j];
        }
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) m_Const.lightLocal[r][c] = lights[c][r];
            m_Const.lightLocal[r][3] = 0.0f;
        }
    }

    // c12: base colour in PS2 range (128 = 1.0). Alpha comes from the GS ALPHA register's FIX
    // when the blend uses it, otherwise from the object.
    for (int i = 0; i < 3; i++) {
        float v = F32(data + 0x40 + 4 * i);
        m_Const.baseColor[i] = v < 128.0f ? v / 128.0f : 1.0f;
    }
    uint64_t alpha = CGs::GetReg(gs::ALPHA_1);
    int a = ((alpha >> 4) & 3) == 2 ? (int)(int8_t)(alpha >> 32) : (int)(int8_t)(int)F32(data + 0x4c);
    m_Const.baseColor[3] = (a < 0 ? -a : a) / 128.0f;

    // c13: S/T offsets and a depth offset (Z scaled by -1/65536).
    m_Const.stzOffset[0] = F32(data + 0x50);
    m_Const.stzOffset[1] = F32(data + 0x54);
    m_Const.stzOffset[2] = F32(data + 0x58) * -1.5258789e-05f;
    m_Const.stzOffset[3] = F32(data + 0x5c);
}

void CVertex::Unpack(uint32_t code, const uint8_t *data, size_t bytes)
{
    (void)bytes;
    uint32_t fmt = (code >> 24) & 0x0f, num = (code >> 16) & 0xff, addr = code & 0x3ff;
    bool flg = (code & 0x8000) != 0;
    switch (fmt) {
    case 0x5: if (flg) m_pUVs = data; break;                  // V2-16 texture coordinates
    case 0x9: if (flg) m_pNormals = data; break;              // V3-16 normals
    case 0xc:                                                 // V4-32
        if (!flg) {
            if (num == 6) Object(data);
            else if (num == 0x1c) Camera(data);
        } else if (addr == 0) {
            m_pHeader = data;                                 // packet header (+ positions)
            // Run (at the next packet's MSCNT) reads the model's entry: request it now
            uint32_t handle = U32(data + 0x0c);
            if (handle && handle <= m_Models.size()) __builtin_prefetch(&m_Models[handle - 1]);
        }
        break;
    }
}

static uint64_t FrameClock() { return (uint64_t)CGraphic::GetFrameSerial() * 1000 / 60; }
uint64_t (*CVertex::s_pDebugClock)() = FrameClock;

void CVertex::Run()
{
    if (!m_pHeader) return;
    const uint8_t *hdr = m_pHeader;
    // The model's material through the GS emulator: PRIM from the GIF tag, TEX0, CLAMP.
    uint32_t tag[4], tex0[2], clamp[2];
    memcpy(tag, hdr + 0x40, 16);
    memcpy(tex0, hdr + 0x20, 8);
    memcpy(clamp, hdr + 0x28, 8);
    // Consecutive packets of one model (or of models sharing a material) write the same values;
    // nothing else changed the GS or CGraphic state in between when the serial and frame match.
    uint32_t material[8] = {tag[0], tag[1], tag[2], tag[3], tex0[0], tex0[1], clamp[0], clamp[1]};
    if (CGs::WriteSerial() != m_lastMaterialSerial || CGraphic::GetFrameSerial() != m_lastMaterialFrame ||
        memcmp(material, m_lastMaterial, sizeof(material))) {
        CGs::Write(gs::PRIM, tag, gs::FMT_TAG_PRIM);
        CGs::Write(gs::TEX0_1, tex0, gs::FMT_REG64);
        CGs::Write(gs::CLAMP_1, clamp, gs::FMT_REG64);
        memcpy(m_lastMaterial, material, sizeof(material));
        m_lastMaterialSerial = CGs::WriteSerial();
        m_lastMaterialFrame = CGraphic::GetFrameSerial();
    }

    uint32_t handle = U32(hdr + 0x0c);
    if (!handle || handle > m_Models.size() || !m_pObject) return;
    const Model &m = m_Models[handle - 1];
    if (!m.used) return;

    // Shader by the object's lighting mode (+0x6C, just past the 6-qword object block).
    uint32_t flags = U32(hdr + 4);
    MeshShader shader;
    switch (U32(m_pObject + 0x6c)) {
    case 0: case 3: shader = (flags >> 12) & 1 ? MESH_DIFFUSE : MESH_LIGHT; break;
    case 2: case 5: shader = MESH_STZOFS; break;
    case 6: case 7: shader = MESH_SPECULAR; break;
    default: return;
    }
    // debugging: RECOMP_SKIP_TEX_SEQ="<start ms>:<h>,<h>,..." skips meshes using texture handle
    // h[k] during second k after start
    if (const char *seq = CACHED_ENV("RECOMP_SKIP_TEX_SEQ")) {
        uint64_t start = strtoull(seq, nullptr, 10), now = s_pDebugClock();
        if (now >= start) {
            uint64_t k = (now - start) / 1000;
            const char *p = strchr(seq, ':');
            for (uint64_t i = 0; p && i < k; i++) p = strchr(p + 1, ',');
            if (p && strtoul(p + 1, nullptr, 0) == (tex0[0] & 0x3fff)) return;
        }
    }
    // debugging: RECOMP_SKIP_DIFFUSE_SEQ="<start ms>:<step>" skips diffuse draws [k*step, (k+1)*step)
    // of each frame during second k after start
    if (shader == MESH_DIFFUSE) {
        static uint32_t serial, index;
        if (CGraphic::GetFrameSerial() != serial) { serial = CGraphic::GetFrameSerial(); index = 0; }
        uint32_t i = index++;
        if (const char *seq = CACHED_ENV("RECOMP_SKIP_DIFFUSE_SEQ")) {
            uint64_t start = strtoull(seq, nullptr, 10), now = s_pDebugClock();
            uint32_t step = (uint32_t)strtoul(strchr(seq, ':') + 1, nullptr, 10);
            if (now >= start && i / step == (now - start) / 1000) return;
        }
    }
    // RECOMP_TRACE_DIFFUSE=<start ms>: log one frame's worth of diffuse draws from then on
    if (shader == MESH_DIFFUSE && CACHED_ENV("RECOMP_TRACE_DIFFUSE") && s_pDebugClock() >= strtoull(CACHED_ENV("RECOMP_TRACE_DIFFUSE"), nullptr, 10)) {
        static int n;
        if (n++ < 400)
            fprintf(stderr, "diffuse tex0 %08x%08x prim %08x base %.2f %.2f %.2f %.2f type %u verts %zu\n", tex0[1], tex0[0],
                    (tag[1] >> 15) & 0x7ff, m_Const.baseColor[0], m_Const.baseColor[1], m_Const.baseColor[2],
                    m_Const.baseColor[3], m.type, m.stripPrims.size());
    }
    if (m.type == 3) {
        CGraphic::DrawMesh(m.pMesh, shader, d3d::TRIANGLELIST, 0, m.listPrims, m.listCull, m_Const);
    } else {
        CGraphic::DrawMeshStrips(m.pMesh, shader, m.stripPrims.data(), m.stripCull.data(), m.stripPrims.size(), m_Const);
    }
}
