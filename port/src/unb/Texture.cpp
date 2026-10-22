// Texture.cpp - CTexture on top of the SDL3 GPU CGraphic
//
// Behaviour follows the original unb/Texture.cpp; the implementation is new. See Texture.h.

#include "Texture.h"
#include <cstdlib>
#include <cstring>

// getenv once per call site (debug hooks run in hot paths)
#define CACHED_ENV(name) ([]() -> const char * { static const char *v_ = getenv(name); return v_; }())

// ---------------------------------------------------------------------------------------------
// PS2 data decoding

namespace ps2 {

int BitsPerPixel(uint8_t psm)
{
    switch (psm & 0x3f) {
    case PSMCT32: return 32;
    case PSMCT24: return 24;
    case PSMCT16: case PSMCT16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    default: return 0;
    }
}

bool IsIndexed(uint8_t psm)
{
    int bpp = BitsPerPixel(psm);
    return bpp == 8 || bpp == 4;
}

uint32_t ImageSize(uint8_t psm, int width, int height)
{
    auto align16 = [](int v) { return (uint32_t)((v + 15) & ~15); };
    int n = width * height;
    switch (BitsPerPixel(psm)) {
    case 32: return align16(n * 4);
    case 24: return align16((n + 5) * 3);    // as computed by the original
    case 16: return align16(n * 2);
    case 8:  return align16(n);
    case 4:  return align16(n / 2);
    default: return 0;
    }
}

const uint8_t *ClutData(const TXMHEADER *p)
{
    return reinterpret_cast<const uint8_t *>(p) + sizeof(TXMHEADER);
}

const uint8_t *PixelData(const TXMHEADER *p)
{
    // clutState holds the CLUT pixel mode until the record is loaded; clutPsm keeps a copy.
    uint8_t mode = (p->clutState == 0x0f || p->clutState == 0x05) ? p->clutPsm : p->clutState;
    uint32_t clut = mode == 0xff ? 0 : ImageSize(mode, p->clutWidth, p->clutHeight);
    return ClutData(p) + clut;
}

uint32_t RecordSize(const TXMHEADER *p)
{
    return (uint32_t)(PixelData(p) - reinterpret_cast<const uint8_t *>(p)) + ImageSize(p->psm, p->width, p->height);
}

void ConvertClut(const uint8_t *src, int count, uint8_t *dst)
{
    for (int i = 0; i < count; i++) {
        int s = i;
        if (count >= 256) {
            // CSM1: within each block of 32 entries, entries 8-15 and 16-23 are swapped.
            switch ((i >> 3) & 3) {
            case 1: s = i + 8; break;
            case 2: s = i - 8; break;
            }
        }
        const uint8_t *e = src + s * 4;
        dst[i * 4 + 0] = e[0];
        dst[i * 4 + 1] = e[1];
        dst[i * 4 + 2] = e[2];
        // PS2 alpha: 0x80 = opaque. The original scales |signed alpha| * 255 / 128 (MulDiv).
        int a = std::abs((int)(int8_t)e[3]);
        dst[i * 4 + 3] = (uint8_t)SDL_min((a * 255 + 64) / 128, 255);
    }
}

static inline uint8_t Index4(const uint8_t *row, int i)
{
    return (row[i >> 1] >> ((i & 1) ? 4 : 0)) & 0x0f;      // low nibble first
}

bool Decode(const uint8_t *src, int bpp, int width, int height, int x, int y, int scaleX, int scaleY,
            const uint8_t *clut, TexFormat out, uint8_t *dst, uint32_t dstPitch)
{
    // The original only handles 4, 8 and 32 bits and rejects 16/24-bit data; no game data uses
    // those (checked across cddata).
    if (bpp != 4 && bpp != 8 && bpp != 32) return false;
    if (out == TEXFMT_P8 && bpp == 32) return false;
    if (out == TEXFMT_BGRA8 && bpp != 32 && !clut) return false;

    int dx0 = x / scaleX, dy0 = y / scaleY;
    int dw = (x + width) / scaleX - dx0, dh = (y + height) / scaleY - dy0;
    uint32_t srcRowBytes = (uint32_t)(width * bpp / 8);
    for (int r = 0; r < dh; r++) {
        const uint8_t *row = src + (size_t)(r * scaleY) * srcRowBytes;
        if (out == TEXFMT_P8) {
            uint8_t *d = dst + (size_t)(dy0 + r) * dstPitch + dx0;
            for (int c = 0; c < dw; c++) d[c] = bpp == 4 ? Index4(row, c * scaleX) : row[c * scaleX];
            continue;
        }
        uint8_t *d = dst + (size_t)(dy0 + r) * dstPitch + dx0 * 4;
        for (int c = 0; c < dw; c++, d += 4) {
            const uint8_t *rgba;
            if (bpp == 32) rgba = row + c * scaleX * 4;     // alpha copied as stored, like the original
            else rgba = clut + (bpp == 4 ? Index4(row, c * scaleX) : row[c * scaleX]) * 4;
            d[0] = rgba[2];
            d[1] = rgba[1];
            d[2] = rgba[0];
            d[3] = rgba[3];
        }
    }
    return true;
}

} // namespace ps2

// ---------------------------------------------------------------------------------------------
// CTexture

static constexpr uint8_t kLoaded = 0x0f;
static constexpr uint8_t kNoClutMarked = 0x05;

bool CTexture::Init()
{
    m_Images.assign(CGraphic::kMaxTextures, Image());
    m_Cluts.assign(CGraphic::kMaxTextures, Clut());
    return true;
}

void CTexture::Term()
{
    for (int i = 0; i < (int)m_Images.size(); i++) if (m_Images[i].used) FreeImage(i);
    for (int i = 0; i < (int)m_Cluts.size(); i++) if (m_Cluts[i].used) FreeClut(i);
}

int CTexture::AllocImage()
{
    for (int i = 0; i < (int)m_Images.size(); i++) if (!m_Images[i].used) return i;
    return -1;
}

int CTexture::AllocClut()
{
    for (int i = 0; i < (int)m_Cluts.size(); i++) if (!m_Cluts[i].used) return i;
    return -1;
}

CGraphicTexture *CTexture::GetTexture(int nHandle) const
{
    return nHandle >= 0 && nHandle < (int)m_Images.size() && m_Images[nHandle].used ? m_Images[nHandle].pTex : nullptr;
}

uint32_t CTexture::GetPalette(int nHandle) const
{
    return nHandle >= 0 && nHandle < (int)m_Cluts.size() && m_Cluts[nHandle].used ? m_Cluts[nHandle].nPalID
                                                                                     : CGraphic::kNoPalette;
}

bool CTexture::IsLoaded(const TXMHEADER *p, bool bAllocOnly)
{
    if (p->state != kLoaded) return false;
    uint8_t cs = p->clutState;
    if (cs == kLoaded || cs == kNoClutMarked) return true;
    if (cs == 0xff) return false;
    return !(CGraphic::GetTextureFormat() == TEXFMT_P8 || bAllocOnly);      // as Load
}

bool CTexture::Load(TXMHEADER *p, bool bAllocOnly, int nVariant)
{
    (void)nVariant;
    if (p->state != kLoaded) {
        uint8_t state = p->state;
        if (!LoadImage(p, bAllocOnly)) return false;
        m_Images[p->handle].savedState = state;
        p->state = kLoaded;
    }
    uint8_t cs = p->clutState;
    if (cs == kLoaded || cs == kNoClutMarked) return true;
    if (cs == 0xff) { p->clutState = kNoClutMarked; return true; }
    // In P8 mode the palette is needed at draw time; in BGRA mode the image was already expanded
    // through the CLUT, so it is only kept when pixels will be uploaded later (bAllocOnly).
    if (CGraphic::GetTextureFormat() == TEXFMT_P8 || bAllocOnly) {
        if (!LoadClut(p, bAllocOnly)) return false;
        m_Cluts[p->clutHandle].savedState = cs;
        p->clutState = kLoaded;
    }
    return true;
}

void CTexture::Unload(TXMHEADER *p)
{
    if (!p) return;
    if (p->state == kLoaded) {
        int h = p->handle;
        p->state = m_Images[h].savedState;
        p->handle = m_Images[h].savedHandle;
        FreeImage(h);
    }
    if (p->clutState == kLoaded) {
        int h = p->clutHandle;
        p->clutState = m_Cluts[h].savedState;
        p->clutHandle = m_Cluts[h].savedHandle;
        FreeClut(h);
    } else if (p->clutState == kNoClutMarked) {
        p->clutState = 0xff;
    }
}

bool CTexture::LoadImage(TXMHEADER *p, bool bAllocOnly)
{
    if (p->psm == 0xff || p->psm == 0xfe) return false;
    int bpp = ps2::BitsPerPixel(p->psm);
    TexFormat fmt = ps2::IsIndexed(p->psm) && CGraphic::GetTextureFormat() == TEXFMT_P8 ? TEXFMT_P8 : TEXFMT_BGRA8;

    // Power-of-two size clamped to the device limit, as D3DXCheckTextureRequirements did. The
    // PS2 GS also wraps textures at power-of-two sizes, so UVs depend on it.
    uint32_t pw = CGraphic::PowerOfTwo((uint32_t)p->width), ph = CGraphic::PowerOfTwo((uint32_t)p->height);
    uint32_t tw = SDL_min(pw, CGraphic::GetMaxTextureWidth()), th = SDL_min(ph, CGraphic::GetMaxTextureHeight());

    int h = AllocImage();
    if (h < 0) return false;
    Image &img = m_Images[h];
    img.pTex = CGraphic::CreateTexture(tw, th, fmt);
    if (!img.pTex) return false;
    img.used = true;
    img.nWidth = tw;
    img.nHeight = th;
    img.nFormat = fmt;
    img.scaleX = (uint8_t)(pw / tw);
    img.scaleY = (uint8_t)(ph / th);
    CGraphic::SetTextureUVSize(img.pTex, tw * img.scaleX, th * img.scaleY);   // UVs address the full-size image

    if (!bAllocOnly) {
        // BGRA output of indexed data needs the converted CLUT.
        uint8_t clut[256 * 4];
        const uint8_t *pClut = nullptr;
        if (fmt == TEXFMT_BGRA8 && ps2::IsIndexed(p->psm) && p->clutState != 0xff) {
            ps2::ConvertClut(ps2::ClutData(p), SDL_min(p->clutWidth * p->clutHeight, 256), clut);
            pClut = clut;
        }
        uint32_t pitch = 0;
        auto *dst = static_cast<uint8_t *>(CGraphic::LockTexture(img.pTex, &pitch));
        bool ok = dst && ps2::Decode(ps2::PixelData(p), bpp, p->width, p->height, 0, 0, img.scaleX, img.scaleY,
                                     pClut, fmt, dst, pitch);
        CGraphic::UnlockTexture(img.pTex);
        if (!ok) { FreeImage(h); return false; }
        if (CACHED_ENV("RECOMP_DUMP_TEX")) {        // debugging: tex/<handle>_<w>x<h>.bmp
            std::vector<uint8_t> buf((size_t)tw * th * 4);
            uint8_t dclut[256 * 4];
            if (ps2::IsIndexed(p->psm) && p->clutState != 0xff)
                ps2::ConvertClut(ps2::ClutData(p), SDL_min(p->clutWidth * p->clutHeight, 256), dclut);
            ps2::Decode(ps2::PixelData(p), bpp, p->width, p->height, 0, 0, img.scaleX, img.scaleY,
                        ps2::IsIndexed(p->psm) ? dclut : nullptr, TEXFMT_BGRA8, buf.data(), tw * 4);
            if (SDL_Surface *sf = SDL_CreateSurfaceFrom((int)tw, (int)th, SDL_PIXELFORMAT_BGRA32, buf.data(), (int)tw * 4)) {
                char path[64];
                SDL_CreateDirectory("tex");
                static int dumpN; SDL_snprintf(path, sizeof(path), "tex/%05d_%03d_%dx%d_psm%02x.bmp", dumpN++, h, p->width, p->height, p->psm);
                SDL_SaveBMP(sf, path);
                SDL_DestroySurface(sf);
            }
        }
    }

    img.savedHandle = p->handle;
    p->handle = (int16_t)h;
    CGraphic::SetManagedTexture((uint32_t)h, img.pTex);
    return true;
}

bool CTexture::LoadClut(TXMHEADER *p, bool bKeep)
{
    if (p->clutState == 0xff || p->clutState == 0xfe) return false;
    int count = SDL_min(p->clutWidth * p->clutHeight, 256);
    int h = AllocClut();
    if (h < 0) return false;
    Clut &c = m_Cluts[h];
    c.rgba.assign(256 * 4, 0);
    ps2::ConvertClut(ps2::ClutData(p), count, c.rgba.data());
    if (CGraphic::GetTextureFormat() == TEXFMT_P8) {
        c.nPalID = CGraphic::AllocPalette();
        if (c.nPalID >= CGraphic::kNoPalette || !CGraphic::UploadPalette(c.nPalID, c.rgba.data(), 256)) return false;
    } else if (!bKeep) {
        return false;
    }
    c.used = true;
    c.savedHandle = p->clutHandle;
    p->clutHandle = (int16_t)h;
    CGraphic::SetManagedPalette((uint32_t)h, c.nPalID);
    return true;
}

void CTexture::FreeImage(int h)
{
    Image &img = m_Images[h];
    if (!img.used) return;
    CGraphic::SetManagedTexture((uint32_t)h, nullptr);
    CGraphic::DestroyTexture(img.pTex);
    img = Image();
}

void CTexture::FreeClut(int h)
{
    Clut &c = m_Cluts[h];
    if (!c.used) return;
    CGraphic::SetManagedPalette((uint32_t)h, CGraphic::kNoPalette);
    CGraphic::FreePalette(c.nPalID);
    c = Clut();
}

bool CTexture::Update(const TXMHEADER *p, const void *pPixels, int x, int y)
{
    if (p->state != kLoaded || !GetTexture(p->handle)) return false;
    Image &img = m_Images[p->handle];
    const uint8_t *clut = nullptr;
    if (img.nFormat == TEXFMT_BGRA8 && ps2::IsIndexed(p->psm)) {
        if (p->clutState != kLoaded) return false;
        clut = m_Cluts[p->clutHandle].rgba.data();
    }
    uint32_t pitch = 0;
    auto *dst = static_cast<uint8_t *>(CGraphic::LockTexture(img.pTex, &pitch));
    bool ok = dst && ps2::Decode(static_cast<const uint8_t *>(pPixels), ps2::BitsPerPixel(p->psm), p->width, p->height,
                                 x, y, img.scaleX, img.scaleY, clut, img.nFormat, dst, pitch);
    CGraphic::UnlockTexture(img.pTex);
    return ok;
}
