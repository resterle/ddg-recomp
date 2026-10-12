// Gs.cpp - PS2 GS emulation on top of the SDL3 GPU CGraphic
//
// Behaviour follows the original (see Gs.h for addresses); the implementation is new.

#include "Gs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// getenv once per call site (debug hooks run in hot paths)
#define CACHED_ENV(name) ([]() -> const char * { static const char *v_ = getenv(name); return v_; }())

using namespace d3d;

namespace {

struct State {
    uint64_t regs[0x63] = {};
    int count = 0;                      // vertices since the last PRIM (0x67323c)
    D3DCOLOR color = 0xff000000;        // RGBAQ converted to D3DCOLOR (0x673240)
    float u = 0, v = 0;                 // UV after NormalizeUV (0x673244/48)
    // IMAGE transfer in progress
    uint32_t imageRemaining = 0;        // qwords still expected
    std::vector<uint8_t> image;
};

State s;

inline uint64_t Lo64(const uint32_t *d) { return d[0] | (uint64_t)d[1] << 32; }

// PS2 alpha (0x80 = 1.0) to 0-255, as the original: MulDiv(|(int8)a|, 255, 128).
inline uint32_t Alpha255(uint8_t a)
{
    int v = (int)(int8_t)a;
    v = v < 0 ? -v : v;
    v = (v * 255 + 64) / 128;
    return (uint32_t)(v > 255 ? 255 : v);
}

// Coordinates are in a 640x480 layout; the original scaled them when scrwidth/scrheight differ.
inline bool ScaleScreen()
{
    return CGraphic::m_Config.scrwidth != 640 || CGraphic::m_Config.scrheight != 480;
}

// FST (bit 8 of PRIM) selects UV coordinates: 2D drawing, which uses the configured modulate op.
void SetCombineOps()
{
    bool fst = (s.regs[gs::PRIM] >> 8) & 1;
    CGraphic::SetTextureStageState(0, TSS_COLOROP, fst ? CGraphic::GetTextureModulate() : (uint32_t)TOP_MODULATE);
    CGraphic::SetTextureStageState(0, TSS_ALPHAOP, TOP_MODULATE);
}

} // namespace

GsStats CGs::stats;
int CGs::traceKicks;
uint64_t CGs::s_writes;

void CGs::Reset()
{
    s = State();
    s_writes++;
}

uint64_t CGs::GetReg(uint32_t reg)
{
    return reg < 0x63 ? s.regs[reg] : 0;
}

void CGs::WriteReg(uint32_t reg, uint64_t value)
{
    uint32_t d[2] = {(uint32_t)value, (uint32_t)(value >> 32)};
    Write(reg, d, gs::FMT_REG64);
}

// Alpha test and write masks from TEST_1 and ZBUF_1. With ATST = NEVER the GS still writes
// what AFAIL selects: FB_ONLY (colour), ZB_ONLY (depth; games clear Z with it) or RGB_ONLY. The
// PC port passed D3DCMP_NEVER and drew nothing; this follows the GS (a never-passing draw that
// discards every fragment also misbehaved on Apple GPUs).
static void ApplyWriteMasks()
{
    uint64_t t = s.regs[gs::TEST_1];
    bool zmsk = (s.regs[gs::ZBUF_1] >> 32) & 1;
    bool never = (t & 1) && ((t >> 1) & 7) == 0;
    uint32_t afail = (t >> 12) & 3;
    bool alphaTest = (t & 1) && !never;
    uint32_t colorWrite = 15;
    bool zWrite = !zmsk;
    if (never) {
        if (afail == 1) zWrite = false;                             // FB_ONLY
        else if (afail == 2) colorWrite = 0;                        // ZB_ONLY
        else if (afail == 3) { colorWrite = 7; zWrite = false; }    // RGB_ONLY
    }
    CGraphic::SetRenderState(RS_ALPHATESTENABLE, alphaTest);
    CGraphic::SetRenderState(RS_COLORWRITEENABLE, colorWrite);
    CGraphic::SetRenderState(RS_ZWRITEENABLE, zWrite);
}

void CGs::Write(uint32_t reg, const uint32_t *d, gs::Format fmt)
{
    using namespace gs;
    if (reg >= 0x63) return;
    stats.regs++;
    s_writes++;
    if (CACHED_ENV("RECOMP_TRACE_REGS")) {
        static uint64_t seen[0x63]; static bool any[0x63];
        uint64_t v = fmt == FMT_PACKED ? ((uint64_t)d[1] << 32 | d[0]) : Lo64(d);
        if (!any[reg] || (seen[reg] != v && reg != 1 && reg != 3 && reg != 5 && reg != 0 && reg != 2 && reg != 4)) {
            any[reg] = true; seen[reg] = v;
            fprintf(stderr, "gsreg %02x = %016llx (fmt %d)\n", reg, (unsigned long long)v, (int)fmt);
        }
    }
    bool packed = fmt == FMT_PACKED;

    switch (reg) {
    case PRIM: {
        uint64_t prim = fmt == FMT_TAG_PRIM ? (Lo64(d) >> 47) & 0x7ff : Lo64(d);
        s.regs[PRIM] = prim;
        s.count = 0;
        static const PrimitiveType kType[7] = {POINTLIST, LINELIST, LINESTRIP, TRIANGLELIST,
                                               TRIANGLESTRIP, TRIANGLEFAN, TRIANGLESTRIP /* sprite */};
        if ((prim & 7) < 7) CGraphic::SetPrimitiveType(kType[prim & 7]);
        CGraphic::SetRenderState(RS_SHADEMODE, (prim >> 3) & 1 ? SHADE_GOURAUD : SHADE_FLAT);
        // Every PRIM unbinds the texture; TEX0 has to follow to texture the primitive.
        CGraphic::SetTexture(CGraphic::kMaxTextures, 0, CGraphic::kNoPalette);
        if (CGraphic::m_Config.fog) CGraphic::SetRenderState(RS_FOGENABLE, (prim >> 5) & 1);
        CGraphic::SetRenderState(RS_ALPHABLENDENABLE, (prim >> 6) & 1);
        CGraphic::SetRenderState(RS_EDGEANTIALIAS, (prim >> 7) & 1);
        if (!((prim >> 4) & 1)) SetCombineOps();        // TME off
        return;
    }
    case RGBAQ: {
        uint32_t rgba = packed ? (d[0] & 0xff) | (d[1] & 0xff) << 8 | (d[2] & 0xff) << 16 | d[3] << 24 : d[0];
        s.regs[RGBAQ] = packed ? (s.regs[RGBAQ] & ~0xffffffffull) | rgba : Lo64(d);
        // R, G, B stay in PS2 range (0x80 = 1.0, scaled by MODULATE2X); alpha is rescaled.
        s.color = Alpha255((uint8_t)(rgba >> 24)) << 24 | (rgba & 0xff) << 16 | (rgba & 0xff00) | ((rgba >> 16) & 0xff);
        return;
    }
    case ST:
        s.regs[ST] = Lo64(d);
        if (packed) s.regs[RGBAQ] = (s.regs[RGBAQ] & 0xffffffffull) | (uint64_t)d[2] << 32;   // Q
        return;
    case UV: {
        uint32_t u, v;
        bool screenSpace;
        if (packed) {
            u = d[0] & 0x3fff;
            v = d[1] & 0x3fff;
            screenSpace = (d[1] >> 14) != 0;
            s.regs[UV] = u | (uint64_t)(d[1] & 0xffff) << 16;
        } else {
            uint64_t r = Lo64(d);
            u = r & 0x3fff;
            v = (r >> 16) & 0x3fff;
            screenSpace = (r >> 30) != 0;
            s.regs[UV] = r;
        }
        s.u = u * 0.0625f;
        s.v = v * 0.0625f;
        // Out-of-range UV bits mark screen-space coordinates (sampling the screen-effect target).
        if (screenSpace && ScaleScreen()) {
            s.u = CGraphic::m_Config.scrwidth * s.u / 640.0f;
            s.v = CGraphic::m_Config.scrheight * s.v / 480.0f;
        }
        CGraphic::NormalizeUV(&s.u, &s.v);
        return;
    }
    case XYZF2:
        // Stored only: the original never kicks a vertex for XYZF2.
        if (packed && (d[3] & 0x8000)) { reg = XYZF3; }
        s.regs[reg] = packed ? (d[0] & 0xffff) | (uint64_t)(d[1] & 0xffff) << 16 | (uint64_t)(d[2] & 0xffffff) << 32 |
                               (uint64_t)(d[3] >> 4 & 0xff) << 56
                             : Lo64(d);
        return;
    case XYZ2: {
        uint64_t xyz = packed ? (d[0] & 0xffff) | (uint64_t)(d[1] & 0xffff) << 16 | (uint64_t)d[2] << 32 : Lo64(d);
        if (packed && (d[3] & 0x8000)) { s.regs[XYZ3] = xyz; return; }   // ADC: no drawing kick
        s.regs[XYZ2] = xyz;
        Kick(xyz);
        return;
    }
    case TEX0_1: {
        uint64_t t = Lo64(d);
        s.regs[TEX0_1] = t;
        // TBP0 is the texture handle, CBP the palette handle and PSM the table (0x0F = CTexture).
        CGraphic::SetTexture((uint32_t)(t & 0x3fff), (uint8_t)((t >> 20) & 0x3f), (uint32_t)((t >> 37) & 0x3fff));
        if (((t >> 35) & 3) == 0) SetCombineOps();      // TFX = MODULATE
        return;
    }
    case CLAMP_1: {
        uint64_t c = Lo64(d);
        s.regs[CLAMP_1] = c;
        static const uint32_t kAddr[4] = {TADDRESS_WRAP, TADDRESS_CLAMP, TADDRESS_CLAMP, TADDRESS_WRAP};
        CGraphic::SetTextureStageState(0, TSS_ADDRESSU, kAddr[c & 3]);
        CGraphic::SetTextureStageState(0, TSS_ADDRESSV, kAddr[(c >> 2) & 3]);
        return;
    }
    case TEX1_1: {
        uint64_t t = Lo64(d);
        s.regs[TEX1_1] = t;
        CGraphic::SetTextureStageState(0, TSS_MAGFILTER, (t >> 5) & 1 ? TEXF_LINEAR : TEXF_POINT);
        uint32_t mmin = (t >> 6) & 7;
        if (mmin <= 1) CGraphic::SetTextureStageState(0, TSS_MINFILTER, mmin ? TEXF_LINEAR : TEXF_POINT);
        return;
    }
    case ALPHA_1: {
        uint64_t a = Lo64(d);
        s.regs[ALPHA_1] = a;
        // Only two blend equations are recognised; anything else keeps the previous blend.
        uint32_t key = (uint32_t)((a & 3) << 12 | ((a >> 2) & 3) << 8 | ((a >> 4) & 3) << 4 | ((a >> 6) & 3));
        if (key == 0x0101) {            // (Cs - Cd) * As + Cd
            CGraphic::SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            CGraphic::SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            CGraphic::SetRenderState(RS_BLENDOP, BLENDOP_ADD);
        } else if (key == 0x1220) {     // Cd * FIX + Cs, treated as additive
            CGraphic::SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            CGraphic::SetRenderState(RS_DESTBLEND, BLEND_ONE);
            CGraphic::SetRenderState(RS_BLENDOP, BLENDOP_ADD);
        }
        return;
    }
    case DTHE:
        s.regs[DTHE] = Lo64(d);
        CGraphic::SetRenderState(RS_DITHERENABLE, d[0] & 1);
        return;
    case TEST_1: {
        uint64_t t = Lo64(d);
        s.regs[TEST_1] = t;
        static const uint32_t kAtst[8] = {CMP_NEVER, CMP_ALWAYS, CMP_LESS, CMP_LESSEQUAL,
                                          CMP_EQUAL, CMP_GREATEREQUAL, CMP_GREATER, CMP_NOTEQUAL};
        CGraphic::SetRenderState(RS_ALPHAFUNC, kAtst[(t >> 1) & 7]);
        if (((t >> 12) & 3) != 3) CGraphic::SetRenderState(RS_ALPHAREF, Alpha255((uint8_t)(t >> 4)));
        CGraphic::SetRenderState(RS_ZENABLE, (t >> 16) & 1);
        // PS2 depth grows towards the viewer; the port stores 1 - z, so the tests flip.
        static const uint32_t kZtst[4] = {CMP_NEVER, CMP_ALWAYS, CMP_LESSEQUAL, CMP_LESS};
        CGraphic::SetRenderState(RS_ZFUNC, kZtst[(t >> 17) & 3]);
        ApplyWriteMasks();
        return;
    }
    case ZBUF_1:
        s.regs[ZBUF_1] = Lo64(d);
        ApplyWriteMasks();
        return;
    case FOGCOL: {  // used by 3D models (CVertex); the 2D path has no fog factor
        uint64_t f = Lo64(d);
        s.regs[FOGCOL] = f;
        CGraphic::SetRenderState(RS_FOGCOLOR, (uint32_t)((f & 0xff) << 16 | (f & 0xff00) | ((f >> 16) & 0xff)));
        return;
    }
    default:
        s.regs[reg] = Lo64(d);
        return;
    }
}

void CGs::Kick(uint64_t xyz)
{
    stats.kicks++;
    if ((s.regs[gs::PRIM] & 0x10) && (s.color >> 24) > stats.maxAlpha) stats.maxAlpha = s.color >> 24;
    uint32_t test = (uint32_t)s.regs[gs::TEST_1];
    // Primitives that can never pass the depth or alpha test are dropped.
    if ((test & 0x10000) && !(test & 0x60000)) return;
    if ((test & 1) && !(test & 0xe) && !(test & 0x3000)) return;   // ATST NEVER, AFAIL KEEP: nothing

    uint64_t ofs = s.regs[gs::XYOFFSET_1];
    // X in 12.4 fixed point; Y counts field lines (the PS2 version rendered interlaced fields),
    // so the original divides by 8 instead of 16 to get frame lines.
    float pos[3];
    pos[0] = (int)((uint32_t)(xyz & 0xffff) - (uint32_t)(ofs & 0xffff)) * 0.0625f;
    pos[1] = (int)((uint32_t)((xyz >> 16) & 0xffff) - (uint32_t)((ofs >> 32) & 0xffff)) * 0.125f;
    if (ScaleScreen()) {
        pos[0] = CGraphic::m_Config.scrwidth * pos[0] / 640.0f;
        pos[1] = CGraphic::m_Config.scrheight * pos[1] / 480.0f;
    }
    uint32_t z = (uint32_t)(xyz >> 32);
    // The original wrote 0.99999994 for z = 0, which its 24-bit depth buffer stored as 1.0. With
    // a float depth buffer the far 3D geometry (sky) would z-fight against it, so use 1.0.
    pos[2] = z == 0 ? 1.0f : z < 0xffffff ? (0xffffff - z) * 5.960465e-08f : 0.0f;

    if (traceKicks > 0) {
        traceKicks--;
        fprintf(stderr, "kick prim %llx pos (%.1f, %.1f, %.6f) color %08x uv (%.3f %.3f) test %llx alpha %llx zbuf %llx tex0 %llx ofs %llx\n",
                (unsigned long long)s.regs[gs::PRIM], pos[0], pos[1], pos[2], s.color, s.u, s.v,
                (unsigned long long)s.regs[gs::TEST_1], (unsigned long long)s.regs[gs::ALPHA_1],
                (unsigned long long)s.regs[gs::ZBUF_1], (unsigned long long)s.regs[gs::TEX0_1],
                (unsigned long long)s.regs[gs::XYOFFSET_1]);
    }
    int prev = s.count++;
    switch (s.regs[gs::PRIM] & 7) {
    case 1:     // line
        if (s.count < 2) { CGraphic::SetVertex(0, s.color, pos, s.u, s.v); return; }
        CGraphic::SetVertex(1, s.color, pos, s.u, s.v);
        CGraphic::DrawPrimitive(2, 1);
        s.count = 0;
        return;
    case 2:     // line strip
        if (s.count < 2) { CGraphic::SetVertex(0, s.color, pos, s.u, s.v); return; }
        if (s.count == 2) CGraphic::SetVertex(1, s.color, pos, s.u, s.v);
        else CGraphic::AddStripVertex(1, s.color, pos, s.u, s.v);
        CGraphic::DrawPrimitive(2, 1);
        return;
    case 4:     // triangle strip: nothing is drawn until the fourth vertex
        if (s.count < 4) { CGraphic::SetVertex(prev, s.color, pos, s.u, s.v); return; }
        if (s.count == 4) {
            CGraphic::SetVertex(3, s.color, pos, s.u, s.v);
            CGraphic::DrawPrimitive(4, 2);
            return;
        }
        CGraphic::AddStripVertex(s.count == 5 ? 3 : (s.count & 1) + 1, s.color, pos, s.u, s.v);
        CGraphic::DrawPrimitive(3, 1);
        return;
    case 6: {   // sprite
        if (s.count < 2) { CGraphic::SetVertex(0, s.color, pos, s.u, s.v); return; }
        CGraphic::SetSpriteRect(s.color, pos, s.u, s.v);
        CGraphic::DrawPrimitive(4, 2);
        // A sprite with ZTST = ALWAYS and Z writes on is how the game clears depth.
        if ((test & 0x10000) && (test & 0x60000) == 0x20000 && !((s.regs[gs::ZBUF_1] >> 32) & 1))
            CGraphic::Clear(CLEAR_ZBUFFER, 0, 1.0f);
        s.count = 0;
        return;
    }
    default:    // points, triangles and fans are not drawn by the original either
        return;
    }
}

void CGs::ImageDone(const uint8_t *data)
{
    stats.images++;
    if ((s.regs[gs::TRXDIR] & 3) != 0) return;          // only host -> local transfers
    uint64_t bb = s.regs[gs::BITBLTBUF], pos = s.regs[gs::TRXPOS], reg = s.regs[gs::TRXREG];
    uint32_t dbp = (uint32_t)(bb >> 32) & 0x3fff, dpsm = (uint32_t)(bb >> 56) & 0x3f;
    int x = (int)(pos >> 32) & 0x7ff, y = (int)(pos >> 48) & 0x7ff;
    int w = (int)reg & 0xfff, h = (int)(reg >> 32) & 0xfff;
    int bits;
    switch (dpsm) {
    case 0x00: case 0x30:                   // PSMCT32 / PSMZ32
    case 0x02: case 0x0a: case 0x32: case 0x3a:     // 16-bit modes
        // 8x2 and 16x16 transfers are CLUTs. (16-bit CLUTs go through the same 32-bit
        // conversion in the original; the game only uses 32-bit ones.)
        if (w == 8 && h == 2) { CGraphic::SetPalette(dbp, data, 16); return; }
        if (w == 16 && h == 16) { CGraphic::SetPalette(dbp, data, 256); return; }
        bits = dpsm == 0x00 || dpsm == 0x30 ? 32 : 16;
        break;
    case 0x01: case 0x31: bits = 24; break;
    case 0x13: case 0x1b: bits = 8; break;
    case 0x14: case 0x24: case 0x2c: bits = 4; break;
    default: return;
    }
    CGraphic::LoadTexture(dbp, data, x, y, w, h, bits);
}

size_t CGs::ProcessGif(const void *data, size_t qwords)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t q = 0;
    auto qw = [&](size_t i) { return reinterpret_cast<const uint32_t *>(p + i * 16); };

    // IMAGE data continuing from the previous DIRECT chunk.
    if (s.imageRemaining) {
        size_t n = SDL_min((size_t)s.imageRemaining, qwords);
        s.image.insert(s.image.end(), p, p + n * 16);
        s.imageRemaining -= (uint32_t)n;
        q += n;
        if (!s.imageRemaining) ImageDone(s.image.data());
    }

    while (q < qwords) {
        const uint32_t *tag = qw(q++);
        uint64_t lo = Lo64(tag), regs = Lo64(tag + 2);
        uint32_t nloop = (uint32_t)(lo & 0x7fff);
        uint32_t flg = (uint32_t)(lo >> 58) & 3;
        uint32_t nreg = (uint32_t)(lo >> 60) & 0xf;
        if (nreg == 0) nreg = 16;

        if (flg == 0) {                                     // PACKED
            if ((lo >> 46) & 1) Write(gs::PRIM, tag, gs::FMT_TAG_PRIM);     // PRE
            for (uint32_t i = 0; i < nloop; i++) {
                for (uint32_t r = 0; r < nreg; r++) {
                    if (q >= qwords) return q;
                    const uint32_t *d = qw(q++);
                    uint32_t reg = (uint32_t)(regs >> (4 * r)) & 0xf;
                    if (reg == gs::AD) Write(d[2] & 0xff, d, gs::FMT_REG64);
                    else if (reg != gs::NOP) Write(reg, d, gs::FMT_PACKED);
                }
            }
        } else if (flg == 1) {                              // REGLIST: 64-bit values, padded to a qword
            uint32_t n = nloop * nreg;
            for (uint32_t k = 0; k < n; k++) {
                if (q >= qwords) return q;
                const uint32_t *d = qw(q) + (k & 1) * 2;
                uint32_t reg = (uint32_t)(regs >> (4 * (k % nreg))) & 0xf;
                if (reg != gs::AD && reg != gs::NOP) Write(reg, d, gs::FMT_REG64);
                if (k & 1) q++;
            }
            if (n & 1) q++;
        } else {                                            // IMAGE
            // The original assumed the pixels always arrive in the next DIRECT chunk; this
            // accepts them in the same chunk or spread over several.
            size_t n = SDL_min((size_t)nloop, qwords - q);
            s.image.assign(p + q * 16, p + (q + n) * 16);
            s.imageRemaining = nloop - (uint32_t)n;
            q += n;
            if (!s.imageRemaining && nloop) ImageDone(s.image.data());
        }
    }
    return q;
}
