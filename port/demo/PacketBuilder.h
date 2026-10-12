// PacketBuilder.h - helpers for building GS/GIF packets in the demos.

#pragma once
#include "../src/unb/Gs.h"
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace gs;

// Little GIF packet builder.
struct Gif {
    std::vector<uint64_t> q;    // pairs of 64-bit halves = qwords

    void Tag(uint32_t nloop, bool eop, uint32_t flg, std::initializer_list<uint8_t> regs, uint64_t prim = 0, bool pre = false)
    {
        uint64_t r = 0;
        int n = 0;
        for (uint8_t reg : regs) r |= (uint64_t)reg << (4 * n++);
        q.push_back(nloop | (uint64_t)eop << 15 | (uint64_t)pre << 46 | (prim & 0x7ff) << 47 | (uint64_t)flg << 58 |
                    (uint64_t)(n & 15) << 60);
        q.push_back(r);
    }
    void AD(uint8_t reg, uint64_t value) { q.push_back(value); q.push_back(reg); }
    void Raw(uint64_t lo, uint64_t hi) { q.push_back(lo); q.push_back(hi); }
    void Packed4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { Raw(a | (uint64_t)b << 32, c | (uint64_t)d << 32); }
    size_t Qwords() const { return q.size() / 2; }
};

// Screen layout constants: X in 12.4 with offset, Y in field lines (frame y / 2) with offset.
constexpr uint32_t OFX = 1728 << 4, OFY = 1800 << 4;
static inline uint32_t GX(float x) { return OFX + (uint32_t)(x * 16); }
static inline uint32_t GY(float y) { return OFY + (uint32_t)(y * 8); }
static inline uint64_t XYZ(float x, float y, uint32_t z = 1) { return GX(x) | (uint64_t)GY(y) << 16 | (uint64_t)z << 32; }
static inline uint64_t UVr(float u, float v) { return (uint64_t)(u * 16) | (uint64_t)(v * 16) << 16; }
static inline uint64_t RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return r | g << 8 | b << 16 | (uint64_t)a << 24; }

static inline uint64_t PrimReg(uint32_t type, bool iip, bool tme, bool abe, bool fst)
{
    return type | iip << 3 | tme << 4 | abe << 6 | fst << 8;
}

static inline uint64_t Tex0(uint32_t tbp, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cbp)
{
    return tbp | (uint64_t)psm << 20 | (uint64_t)tw << 26 | (uint64_t)th << 30 | (uint64_t)cbp << 37;  // TFX = MODULATE
}

// Host -> GS image transfer, as sceGsSetDefLoadImage + IMAGE tag.
static inline void Upload(Gif &g, uint32_t dbp, uint32_t dpsm, uint32_t w, uint32_t h, const void *data, size_t bytes)
{
    g.Tag(4, false, 0, {AD});
    g.AD(BITBLTBUF, (uint64_t)dbp << 32 | (uint64_t)dpsm << 56);
    g.AD(TRXPOS, 0);
    g.AD(TRXREG, w | (uint64_t)h << 32);
    g.AD(TRXDIR, 0);
    size_t qw = (bytes + 15) / 16;
    g.Tag((uint32_t)qw, false, 2, {});
    std::vector<uint64_t> buf(qw * 2, 0);
    memcpy(buf.data(), data, bytes);
    g.q.insert(g.q.end(), buf.begin(), buf.end());
}

