// Dma.cpp - PS2 DMA (source chain) and VIF1 emulation feeding CGs
//
// Behaviour follows the original (see Dma.h for addresses); the implementation is new.

#include "Dma.h"
#include "Graphic.h"
#include "Gs.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>

namespace {

struct Vif1State {
    uint32_t cl = 1, wl = 1;            // CYCLE
    uint32_t mode = 0, mask = 0, mark = 0;
    uint32_t row[4] = {}, col[4] = {};
    uint32_t base = 0, ofst = 0, tops = 0, top = 0, itops = 0, itop = 0;
    bool dbf = false;
};

Vif1State vif;
IVu1Handler *vu1 = nullptr;
CDma::Handler handlers[CDma::kChannels] = {};
bool handlerEnabled[CDma::kChannels] = {};

inline uint32_t Word(const uint8_t *p, size_t i)
{
    uint32_t w;
    memcpy(&w, p + i * 4, 4);
    return w;
}

} // namespace

DmaStats CDma::stats;

void CDma::SetHandler(int channel, Handler fn, bool enabled)
{
    if (channel < 0 || channel >= kChannels) return;
    handlers[channel] = fn;
    handlerEnabled[channel] = enabled;
}

void CDma::SetVu1Handler(IVu1Handler *pHandler)
{
    vu1 = pHandler;
}

void CDma::Vif1(const uint32_t *tagWords, int nTagWords, const uint8_t *data, size_t words)
{
    size_t cur = 0;     // next payload word in `data`
    int t = 0;          // next VIF code taken from the DMA tag (TTE)
    // Payload always comes from the data stream, even for a code that sat in the tag.
    auto take = [&](size_t n) -> const uint8_t * {
        if (cur + n > words) { cur = words; return nullptr; }
        const uint8_t *p = data + cur * 4;
        cur += n;
        return p;
    };

    while (t < nTagWords || cur < words) {
        if (t >= nTagWords) {           // NOP padding (half of all codes): two words per test
            while (cur + 2 <= words) {
                uint64_t two;
                memcpy(&two, data + cur * 4, 8);
                if (two & 0x7f0000007f000000ull) break;
                cur += 2;
            }
            if (cur >= words) break;
        }
        uint32_t code = t < nTagWords ? tagWords[t++] : Word(data, cur++);
        if ((code & 0x7f000000u) == 0) continue;           // NOP (half of all codes: padding)
        uint32_t cmd = (code >> 24) & 0x7f, imm = code & 0xffff, num = (code >> 16) & 0xff;

        switch (cmd) {
        case 0x00: case 0x06: case 0x10: case 0x11: case 0x13:      // NOP, MSKPATH3, FLUSH*
            break;
        case 0x01: vif.cl = imm & 0xff; vif.wl = (imm >> 8) & 0xff; break;          // STCYCL
        case 0x02: vif.dbf = false; vif.ofst = imm & 0x3ff; vif.tops = vif.base; break;   // OFFSET
        case 0x03: vif.base = imm & 0x3ff; break;                                    // BASE
        case 0x04: vif.itops = imm & 0x3ff; break;                                   // ITOP
        case 0x05: vif.mode = imm & 3; break;                                        // STMOD
        case 0x07: vif.mark = imm; break;                                            // MARK
        case 0x14: case 0x15: case 0x17:                                             // MSCAL, MSCALF, MSCNT
            // Double-buffer swap as on hardware. Only MSCNT ran anything in the original.
            vif.itop = vif.itops;
            vif.top = vif.tops;
            vif.dbf = !vif.dbf;
            vif.tops = vif.base + (vif.dbf ? vif.ofst : 0);
            if (cmd == 0x17 && vu1) { stats.mscnt++; vu1->Run(); }
            break;
        case 0x20:                                                                   // STMASK
            if (const uint8_t *p = take(1)) memcpy(&vif.mask, p, 4);
            break;
        case 0x30: case 0x31:                                                        // STROW, STCOL
            if (const uint8_t *p = take(4)) memcpy(cmd == 0x30 ? vif.row : vif.col, p, 16);
            break;
        case 0x4a:                                                                   // MPG: skip microcode
            take(num * 2);
            break;
        case 0x50: case 0x51:                                                        // DIRECT, DIRECTHL
            if (const uint8_t *p = take(imm * 4)) { stats.directQw += imm; CGs::ProcessGif(p, imm); }
            break;
        default:
            if ((cmd & 0x60) == 0x60) {                                              // UNPACK
                uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, n = num;
                // With WL > CL ("fill" writes) fewer input vectors are consumed.
                if (vif.cl < vif.wl) n = (num / vif.wl) * vif.cl + SDL_min(num % vif.wl, vif.cl);
                size_t bytes = ((32u >> vl) * (vn + 1) * n + 31) / 32 * 4;
                const uint8_t *p = take(bytes / 4);
                if (p && vif.wl <= vif.cl && vu1) { stats.unpacks++; vu1->Unpack(code, p, bytes); }
            }
            break;      // other codes carry no payload
        }
    }
}

void CDma::Send(int channel, const void *pBase, uint32_t tadr, uint32_t chcr, bool bSnapshotKey)
{
    const uint8_t *base = static_cast<const uint8_t *>(pBase);
    uint64_t t0 = SDL_GetTicksNS();
    uint32_t asr[2] = {};
    int asp = 0;
    chcr |= dma::CHCR_STR;
    if (channel == dma::VIF1) CGraphic::BeginFrame();

    if (channel == dma::VIF1) CGraphic::HoldRecordingLock(true);
    // A bad chain must not hang the frame; real chains are far shorter than this.
    for (int guard = 0; guard < (1 << 20) && (chcr & dma::CHCR_STR); guard++) {
        const uint8_t *tag = base + tadr;
        stats.tags++;
        uint64_t lo;
        memcpy(&lo, tag, 8);
        uint32_t qwc = (uint32_t)lo & 0xffff, id = (uint32_t)(lo >> 28) & 7, addr = (uint32_t)(lo >> 32) & 0x7fffffff;
        const uint8_t *madr = tag + 16;
        uint32_t next = tadr;
        bool end = false;

        switch (id) {
        case 0: madr = base + addr; end = true; break;                      // refe
        case 1: next = tadr + 16 + qwc * 16; break;                         // cnt
        case 2: next = addr; break;                                         // next
        case 3: case 4: madr = base + addr; next = tadr + 16; break;        // ref, refs
        case 5:                                                             // call
            next = addr;
            if (asp < 2) asr[asp++] = tadr + 16 + qwc * 16;
            else end = true;                                                // call stack overflow
            break;
        case 6:                                                             // ret
            if (asp > 0) next = asr[--asp];
            else end = true;
            break;
        case 7: end = true; break;                                          // end
        }

        // Prefetch: model packets live all over the game's memory, and each first touch was a
        // cache miss (~150 ns on the R36H). Tags ahead in the chain, and the data the next tag
        // points to (requested before this tag's data is processed). The next tag is read here
        // anyway on the next pass.
        if (!end) {
            const uint8_t *nt = base + next;
            __builtin_prefetch(nt + 64);
            __builtin_prefetch(nt + 128);
            uint64_t nlo;
            memcpy(&nlo, nt, 8);
            uint32_t nid = (uint32_t)(nlo >> 28) & 7, nqwc = (uint32_t)nlo & 0xffff;
            const uint8_t *nd = nid == 0 || nid == 3 || nid == 4 ? base + ((uint32_t)(nlo >> 32) & 0x7fffffff) : nt + 16;
            if (nqwc) for (uint32_t k = 0; k < SDL_min(nqwc * 16, 512u); k += 64) __builtin_prefetch(nd + k);
        }
        // Only channel 1 (VIF1) carried drawing data in the original.
        if (qwc && channel == dma::VIF1) {
            uint32_t tagWords[2];
            memcpy(tagWords, tag + 8, 8);
            Vif1(tagWords, (chcr & dma::CHCR_TTE) ? 2 : 0, madr, (size_t)qwc * 4);
        }
        if (end) chcr &= ~dma::CHCR_STR;
        tadr = next;
    }

    if (channel == dma::VIF1) {
        CGraphic::HoldRecordingLock(false);         // EndFrame may wait
        // RECOMP_TRACE_TICKS: time from the chain start to EndFrame (the frame's packet processing)
        static const bool trace = getenv("RECOMP_TRACE_TICKS") != nullptr;
        static uint64_t sum, n;
        if (trace) {
            sum += SDL_GetTicksNS() - t0;
            if (++n == 300) { SDL_Log("dma: %.2f ms per frame chain (VIF1 send)", sum / 300e6); sum = n = 0; }
        }
        CGraphic::EndFrame(bSnapshotKey);
    }
    if (channel >= 0 && channel < kChannels && handlerEnabled[channel] && handlers[channel]) handlers[channel](channel);
}
