// Gs.h - PS2 GS emulation on top of the SDL3 GPU CGraphic
//
// Replacement for the GS layer of the original PC port:
//   0x40d1b0  GS register writes -> CGraphic vertices and render state
//   0x413d20  GIF tag dispatch for VIF DIRECT/DIRECTHL data
//   0x413900  PACKED tags          0x413ab0  REGLIST tags
//   0x413c50  IMAGE tags           0x40e940  finished image transfers -> textures/palettes
//
// The original kept the GS registers in globals around 0x673080; they live in CGs::State here.
// It is not a full GS: like the original, it draws lines, line strips, triangle strips and
// sprites from XYZ2 kicks, uses UV (not STQ) texture coordinates, and maps only the blend and
// test modes the game uses. Above this layer the original emulated DMA (0x414390) and VIF1
// (0x413df0); those, and the VU1 path used for 3D models, are not part of this file.

#pragma once
#include "Graphic.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gs {

// GS register addresses (A+D / GIF REGS numbering).
enum Reg : uint8_t {
    PRIM = 0x00, RGBAQ = 0x01, ST = 0x02, UV = 0x03, XYZF2 = 0x04, XYZ2 = 0x05, TEX0_1 = 0x06,
    TEX0_2 = 0x07, CLAMP_1 = 0x08, CLAMP_2 = 0x09, FOG = 0x0a, XYZF3 = 0x0c, XYZ3 = 0x0d,
    AD = 0x0e, NOP = 0x0f, TEX1_1 = 0x14, TEX1_2 = 0x15, TEX2_1 = 0x16, TEX2_2 = 0x17,
    XYOFFSET_1 = 0x18, XYOFFSET_2 = 0x19, PRMODECONT = 0x1a, PRMODE = 0x1b, TEXCLUT = 0x1c,
    SCANMSK = 0x22, MIPTBP1_1 = 0x34, MIPTBP1_2 = 0x35, MIPTBP2_1 = 0x36, MIPTBP2_2 = 0x37,
    TEXA = 0x3b, FOGCOL = 0x3d, TEXFLUSH = 0x3f, SCISSOR_1 = 0x40, SCISSOR_2 = 0x41,
    ALPHA_1 = 0x42, ALPHA_2 = 0x43, DIMX = 0x44, DTHE = 0x45, COLCLAMP = 0x46, TEST_1 = 0x47,
    TEST_2 = 0x48, PABE = 0x49, FBA_1 = 0x4a, FBA_2 = 0x4b, FRAME_1 = 0x4c, FRAME_2 = 0x4d,
    ZBUF_1 = 0x4e, ZBUF_2 = 0x4f, BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52, TRXDIR = 0x53,
    HWREG = 0x54, SIGNAL = 0x60, FINISH = 0x61, LABEL = 0x62,
};

// How the data for one register write is laid out.
enum Format {
    FMT_REG64,      // 64-bit register value (A+D and REGLIST)
    FMT_PACKED,     // 128-bit PACKED qword
    FMT_TAG_PRIM,   // PRIM taken from a GIF tag with PRE set (the tag itself is passed)
};

} // namespace gs

struct GsStats { uint32_t kicks, draws, images, regs, maxAlpha; };

class CGs {
public:
    static GsStats stats;
    static int traceKicks;          // >0: log this many vertex kicks (debugging)
    static void Reset();
    // Changes with every register write and Reset: equal values mean no GS state changed between.
    static uint64_t WriteSerial() { return s_writes; }

    // One register write (0x40d1b0). `data` points to 2 (FMT_REG64) or 4 (FMT_PACKED) dwords.
    static void Write(uint32_t reg, const uint32_t *data, gs::Format fmt);
    static void WriteReg(uint32_t reg, uint64_t value);            // convenience for A+D style writes

    // Process GIF tags and their data, as sent through VIF DIRECT/DIRECTHL (0x413d20). `qwords`
    // is the size of this chunk; IMAGE data may continue in the next call. Returns the number
    // of qwords consumed (all of them unless the data is malformed).
    static size_t ProcessGif(const void *data, size_t qwords);

    // Raw register values, for tests and for porting the code above this layer.
    static uint64_t GetReg(uint32_t reg);

private:
    static uint64_t s_writes;
    static void Kick(uint64_t xyz);                                 // XYZ2: add a vertex
    static void ImageDone(const uint8_t *data);                     // 0x40e940
};
