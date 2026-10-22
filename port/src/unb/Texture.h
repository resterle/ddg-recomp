// Texture.h - CTexture on top of the SDL3 GPU CGraphic
//
// Replacement for unb/Texture.cpp (0x40a2e0-0x40ca00 in perfect.exe). CTexture loads the game's
// PS2 texture records (TXM: a 16-byte header, then the CLUT, then the pixels) into GPU textures
// and hands out the handles that the GS emulator later passes to CGraphic::SetTexture with
// nGroup 0x0F.
//
// The original also pre-allocated pools of D3D textures per format and power-of-two size
// (CTexture::Init, 0x40a370) to avoid creating textures at runtime; SDL GPU textures are cheap
// enough to create on demand, so the pools are gone.

#pragma once
#include "Graphic.h"
#include <cstdint>
#include <vector>

// Texture record header as stored in TXM files and DAT containers (little-endian, 16 bytes).
// While a record is loaded, `state`/`clutState` read 0x0F and the handle fields hold CTexture
// handles; Unload restores the original values.
#pragma pack(push, 1)
struct TXMHEADER {
    uint8_t psm;            // GS pixel storage mode of the image (see ps2::PSM); 0xFF/0xFE = none
    uint8_t state;          // 0x0F while loaded
    int16_t width, height;
    int16_t handle;         // image handle while loaded
    uint8_t clutState;      // CLUT pixel mode; 0xFF = no CLUT, 0x0F = loaded, 0x05 = no CLUT (marked)
    uint8_t clutPsm;
    int16_t clutWidth, clutHeight;
    int16_t clutHandle;     // palette handle while loaded
};
#pragma pack(pop)
static_assert(sizeof(TXMHEADER) == 16, "TXM header is 16 bytes");

namespace ps2 {

// GS pixel storage modes (PSM field of TEX0).
enum PSM : uint8_t {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0a,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1b, PSMT4HL = 0x24, PSMT4HH = 0x2c,
};

int BitsPerPixel(uint8_t psm);                              // 32, 24, 16, 8, 4, or 0 if unknown
bool IsIndexed(uint8_t psm);
uint32_t ImageSize(uint8_t psm, int width, int height);     // 0x40b000: bytes, rounded up to 16

// CLUT and pixel data of a record, and the total record size.
const uint8_t *ClutData(const TXMHEADER *p);
const uint8_t *PixelData(const TXMHEADER *p);
uint32_t RecordSize(const TXMHEADER *p);

// 0x40c5e0: PS2 CLUT (RGBA, alpha 0x80 = opaque) to RGBA with alpha 0-255. 256-entry CLUTs are
// stored in the GS CSM1 order (entries 8-15 and 16-23 of every 32 swapped) and are reordered.
void ConvertClut(const uint8_t *src, int count, uint8_t *dstRGBA);

// 0x40b5a0: decode `width` x `height` source texels (bpp 4, 8 or 32) into dst, placed at (x, y)
// in source coordinates. scaleX/scaleY > 1 skip texels for textures larger than the device
// limit. P8 output writes palette indices; BGRA8 output expands indices through clutRGBA.
bool Decode(const uint8_t *src, int bpp, int width, int height, int x, int y, int scaleX, int scaleY,
            const uint8_t *clutRGBA, TexFormat out, uint8_t *dst, uint32_t dstPitch);

} // namespace ps2

class CTexture {
public:
    bool Init();                                            // 0x40a370
    void Term();                                            // 0x40a700
    bool Reset() { return true; }                           // 0x40a7a0: palettes survive with SDL GPU

    // 0x40a800: load the image and (when needed) the CLUT of a record. bAllocOnly creates the
    // texture without uploading pixels (for Update later). nVariant selected one of two
    // equivalent converters in the original (0x40b5a0 / 0x40bc10; CVertex passes 1); the port
    // has one decoder and ignores it.
    bool Load(TXMHEADER *p, bool bAllocOnly = false, int nVariant = 0);
    static bool IsLoaded(const TXMHEADER *p, bool bAllocOnly);   // Load(p, bAllocOnly) would change nothing
    void Unload(TXMHEADER *p);                              // 0x40a8a0
    // 0x40b140: upload new pixels for a loaded record at (x, y), in record size.
    bool Update(const TXMHEADER *p, const void *pPixels, int x = 0, int y = 0);

    // Lookups for tools and for CGraphic-independent callers.
    CGraphicTexture *GetTexture(int nHandle) const;
    uint32_t GetPalette(int nHandle) const;

private:
    struct Image {
        CGraphicTexture *pTex = nullptr;
        uint32_t nWidth = 0, nHeight = 0;       // GPU texture size (power of two)
        uint8_t scaleX = 1, scaleY = 1;         // texel skipping for oversized sources
        TexFormat nFormat = TEXFMT_BGRA8;
        uint8_t savedState = 0;
        int16_t savedHandle = 0;
        bool used = false;
    };
    struct Clut {
        uint32_t nPalID = CGraphic::kNoPalette; // palette in CGraphic (P8 mode only)
        std::vector<uint8_t> rgba;              // converted entries, kept for Update in BGRA mode
        uint8_t savedState = 0;
        int16_t savedHandle = 0;
        bool used = false;
    };

    bool LoadImage(TXMHEADER *p, bool bAllocOnly);              // 0x40a900
    bool LoadClut(TXMHEADER *p, bool bKeep);                    // 0x40b300
    void FreeImage(int nHandle);                                // 0x40b100
    void FreeClut(int nHandle);                                 // 0x40b470
    int AllocImage();
    int AllocClut();

    std::vector<Image> m_Images;
    std::vector<Clut> m_Cluts;
};
