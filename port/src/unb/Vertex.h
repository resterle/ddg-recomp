// Vertex.h - CVertex (3D models) on top of the SDL3 GPU CGraphic
//
// Replacement for unb/Vertex.cpp (0x411030-0x413090) and the VU1 side of the VIF emulation:
//   0x411540  register a model object (up to three model variants), 0x4115b0 unregister
//   0x411600  register one model: load its textures, patch TEX0, build vertex buffers
//   0x411930  per packet: build the vertex buffer, patch the texture handles
//   0x411e20  triangle strips -> D3D strips (split at restart markers)
//   0x4122c0  triangle lists  -> D3D lists (every second triangle re-wound)
//   0x412710  vertex shaders from cddata/vsh (light, diffuse, stzofs, specular)
//   0x412980 / 0x412b60  draw a strip / list model
//   0x40eb50  VIF UNPACK: camera and object constants, stream pointers
//   0x40ef10  VIF MSCNT: set PRIM/TEX0/CLAMP through the GS emulator and draw the model
//
// Model data is a self-contained VIF stream (the DMA chain refs it): per packet
// [MSCNT/NOP][NOP][STCYCL][UNPACK V4-32 header + positions][UNPACK V3-16 normals]
// [UNPACK V2-16 UVs]; the MSCNT at the start of the next packet draws the previous one.
//
// The original wrote the vertex buffer pointer into the packet header (+0x0C). A 64-bit build
// stores a 1-based index into CVertex's model table there instead.

#pragma once
#include "Dma.h"
#include "Graphic.h"
#include "Texture.h"
#include <cstdint>
#include <map>
#include <vector>

class CVertex : public IVu1Handler {
public:
    // Clock (ms) for the RECOMP_SKIP_* debugging hooks; the host can replace it.
    static uint64_t (*s_pDebugClock)();
    bool Init();
    void Term();
    bool Reset() { return true; }       // vertex buffers survive with SDL GPU

    // 0x411540: a model object as stored in the game's *_pdb.dat containers. Offsets inside it
    // are relative to its start (the original relocated them to pointers on load). ppTexList is
    // the texture list the model's texture table indexes into (the matching *_txm.dat records).
    bool RegisterObject(uint8_t *pObject, TXMHEADER *const *ppTexList, int nTexList, CTexture *pTexture);
    void UnregisterObject(uint8_t *pObject);

    // 0x411600: one model (flags, texture table, packet block).
    bool RegisterModel(uint8_t *pModel, TXMHEADER *const *ppTexList, int nTexList, CTexture *pTexture);
    void UnregisterModel(uint8_t *pModel);

    // The model's VIF packet block, as 0x419a40 computed it. The VIF stream to send starts at +0x10.
    static uint8_t *PacketBlock(uint8_t *pModel);
    static uint32_t PacketBlockSize(uint8_t *pModel);

    // Whether [p, p + n) lies inside a registered model's packet block: data that only changes
    // when models are (un)registered (the asynchronous DMA thread reads it in place).
    bool InModelPackets(const uint8_t *p, size_t n) const;

    // IVu1Handler (called by CDma's VIF1 interpreter)
    void Unpack(uint32_t vifcode, const uint8_t *data, size_t bytes) override;
    void Run() override;

private:
    struct Model {
        CGraphicMesh *pMesh = nullptr;
        uint32_t type = 0;                          // 3 = triangle list, 4 = triangle strip
        std::vector<uint32_t> stripCull, stripPrims;
        uint32_t listPrims = 0, listCull = 0;
        uint8_t *pHeader = nullptr;
        bool used = false;
    };

    bool BuildPacket(uint8_t *pHeader, const uint8_t *pNormals, const uint8_t *pUVs);
    void Camera(const uint8_t *data);
    void Object(const uint8_t *data);

    std::vector<Model> m_Models;
    std::map<const uint8_t *, const uint8_t *> m_Blocks;     // registered packet blocks: start -> end
    MeshConstants m_Const = {};
    float m_WorldToScreen[4][4] = {};
    const uint8_t *m_pCamera = nullptr;     // DAT_00673068 + 0x10
    const uint8_t *m_pObject = nullptr;     // DAT_0067306c + 0x10
    const uint8_t *m_pHeader = nullptr;     // DAT_00673070
    const uint8_t *m_pNormals = nullptr;    // DAT_00673074
    const uint8_t *m_pUVs = nullptr;        // DAT_00673078
    // Run's material writes (PRIM tag, TEX0, CLAMP) of the previous packet and the GS write serial
    // and frame after them: the next packet skips them when it would write the same again
    uint32_t m_lastMaterial[8] = {};
    uint64_t m_lastMaterialSerial = ~0ull;
    uint32_t m_lastMaterialFrame = ~0u;
};
