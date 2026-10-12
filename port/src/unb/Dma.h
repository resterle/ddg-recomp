// Dma.h - PS2 DMA (source chain) and VIF1 emulation feeding CGs
//
// Replacement for the layers above the GS in the original PC port:
//   0x4145b0  sceDmaSend(channel, tadr): sets TADR and CHCR.STR, then runs the chain
//   0x414390  DMA source-chain walker; wraps a channel-1 send in BeginFrame/EndFrame
//   0x413df0  VIF1 interpreter: DIRECT/DIRECTHL -> GS, UNPACK/MSCNT -> VU1 (3D models)
//   0x40d180  per-channel completion callback (the emulated DMA interrupt)
//   0x4138f0  sceDmaGetChan
//
// Addresses: PS2 DMA tags hold 31-bit addresses, and the 32-bit original stored real pointers
// there. A 64-bit build cannot, so tag addresses are offsets from the base pointer passed to
// Send. Everything a chain references must live in that one buffer.
//
// VU1: the original did not run VU1 microcode. UNPACK only recorded where the data for each
// format/address lives, and MSCNT started the C++ replacement of the microprogram (0x40ef10,
// drawing through Vertex.cpp). Both are forwarded to an IVu1Handler, which CVertex will
// implement.

#pragma once
#include <cstddef>
#include <cstdint>

class IVu1Handler {
public:
    virtual ~IVu1Handler() = default;
    // UNPACK with WL <= CL. `vifcode` is the VIF code, `data` its payload (already sized).
    virtual void Unpack(uint32_t vifcode, const uint8_t *data, size_t bytes) = 0;
    // MSCNT: run the "microprogram" on what was unpacked since the last call.
    virtual void Run() = 0;
};

namespace dma {

enum ChcrBits : uint32_t {
    CHCR_TTE = 1u << 6,             // also send the tag's upper 64 bits (VIF codes) to the VIF
    CHCR_STR = 1u << 8,             // channel running
};

enum Channel : int { VIF0 = 0, VIF1 = 1, GIF = 2 };

} // namespace dma

// Counters for debugging (reset by the caller)
struct DmaStats { uint32_t tags, directQw, unpacks, mscnt, gifTags; };

class CDma {
public:
    static DmaStats stats;
    static constexpr int kChannels = 10;
    typedef int (*Handler)(int channel);

    // sceDmaSend for source-chain mode. Tag ADDR fields are offsets into `base`. For channel 1
    // the send is one frame: CGraphic::BeginFrame before, EndFrame after (with bSnapshotKey
    // passed through, the original read it from the input state).
    static void Send(int channel, const void *base, uint32_t tadr, uint32_t chcr = dma::CHCR_TTE,
                     bool bSnapshotKey = false);

    // Completion callback, as registered with sceDmaAddIntcHandler on the PS2 (0x40d180).
    static void SetHandler(int channel, Handler fn, bool enabled = true);

    static void SetVu1Handler(IVu1Handler *pHandler);

    // VIF1 code stream (0x413df0), exposed for tests: processes `words` 32-bit words from
    // `data`; `tagWords` (0 or 2) are VIF codes from the DMA tag when TTE is set.
    static void Vif1(const uint32_t *tagWords, int nTagWords, const uint8_t *data, size_t words);
};
