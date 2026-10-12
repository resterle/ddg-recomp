# unb port: SDL3 GPU

New implementations of the PC port layer (`unb/*.cpp`) of Densha de GO! FINAL, targeting SDL3's
GPU API (Metal on macOS, Vulkan on Linux) or OpenGL ES 3 / GL 3.3 (`GraphicGL.cpp`) instead of
Direct3D 8. The recompiled game (`../recomp`) links these files in place of the original modules;
the demos below drive them without the game. Addresses in the tables refer to `perfect.exe`.

Done: `CGraphic` (`src/unb/Graphic.cpp`), `CTexture` (`src/unb/Texture.cpp`), the GS emulator
`CGs` (`src/unb/Gs.cpp`), DMA and VIF1 `CDma` (`src/unb/Dma.cpp`), 3D models `CVertex`
(`src/unb/Vertex.cpp`).

## Build and run

```sh
make                                   # needs SDL3 (Homebrew on macOS, pkg-config elsewhere)
./graphic_demo W                       # windowed, run until closed
./graphic_demo W --frames 120 --snapshot   # writes snapshot/*.bmp on the last frame
./graphic_demo W --txm <cddata>/2d/game2d_vram.dat   # real game textures through CTexture
./gs_demo W --txm <cddata>/2d/game2d_vram.dat        # everything driven by GS packets only
./dma_demo W                                         # one DMA chain per frame, like the game
./model_demo W <cddata>/3d/car_pdb.dat <cddata>/3d/car_txm.dat [--entry N]   # a game model
./cull_test                                          # checks the D3D cull-mode mapping
./txm2png <file.TXM | container.dat> [out_dir]       # decode texture records to PNG (no GPU)
```

`graphic_demo` drives `CGraphic` the way the game's GS emulator (0x40d1b0) does: sprites through
`SetVertex` + `SetSpriteRect`, strips through `AddStripVertex`, paletted and colour textures,
blending, flat shading and the screen-effect feedback (previous frame as texture slot 1).

## CGraphic: what changed against the Direct3D 8 original

| Original (address)                         | Port                                                   |
|--------------------------------------------|--------------------------------------------------------|
| `LoadConfig` reads `HKCU\Software\TAITO\…` (0x402110) | Same keys from `graphics.ini` in SDL's pref path; `W` argument still forces windowed |
| `Init(HWND)` (0x402350)                    | `Init(SDL_Window*)`; caps probing (`GetAdapterFormat` & co.) dropped |
| `Reset` device-lost recovery (0x402ae0)    | No-op: SDL GPU has no device loss                       |
| BeginScene / EndScene / Present (0x40f030, 0x40f090, outside Graphic.cpp) | `BeginFrame` / `EndFrame(bSnapshotKey)` |
| `LoadTexture` + CTexture `LockRect` upload (0x403660, 0x40b5a0) | `LoadTexture(id, w, h)` + `LockTexture` / `UnlockTexture`; CTexture writes into a CPU buffer |
| `SetPalette` via `SetPaletteEntries` (0x403840) | `SetPalette(id, rgba, count)`; palettes in a shader-read atlas |
| P8 textures (hardware palettes)            | R8 index textures, palette lookup and bilinear filtering in the fragment shader |
| `DrawPrimitive` → `DrawPrimitiveUP` per call (0x403e10) | Recorded into a batch, one vertex upload and render pass per flush |
| GS emulator calls `m_pD3DD->SetRenderState` / `SetTextureStageState` directly | `CGraphic::SetRenderState` / `SetTextureStageState` with the same D3D values; pipelines built and cached per state |
| `Clear(4, m_rcBorder, …)` rect clears      | Drawn as black quads                                    |
| Back buffer at scrwidth × scrheight, `m_bScaleScreen` | Scene always rendered off screen at scrwidth × scrheight, scaled into the window with letterboxing |
| `SetGamma` via `SetGammaRamp` (0x403560)   | Applied in the final present shader (`pow(c, 1/gamma)`) |
| `ApplyScreenEffect` (0x404ff0) swaps texture slots 0/1 | Same swap at the end of `EndFrame`              |
| `PrintScreen` → `host0:snapshot/*.bmp` (0x404630) | GPU readback → `snapshot/*.bmp`                  |

Behaviour kept on purpose:
- Half-pixel offset: D3D8 puts pixel centres on integer coordinates, so pre-transformed vertices
  are shifted by 0.5 px to keep the same coverage and texel alignment.
- `DrawPrimitive` disables culling on every call, as the original did (the PS2 GS has no culling).
- MODULATE2X is the default colour op (`texmodulate = 1`), matching PS2 vertex colours where 0x80 = 1.0.
- `AddStripVertex` and `SetSpriteRect` move vertices exactly like the original.

## Shaders and limits

- **Shaders:** MSL source in `src/unb/GraphicShaders.h` (Metal); the same shaders as Vulkan GLSL
  in `src/unb/shaders/scene.glsl`, compiled by `tools/build_spirv.sh` into the checked-in
  `GraphicShadersSPV.h`. `GraphicGL.cpp` has its own GLSL ES. No D3D12 (DXIL) build.
- **Fog** for pre-transformed vertices is ignored (the 2D path never supplies a fog factor).

## CTexture

The game's textures are PS2 texture records ("TXM"), stored alone in `.TXM` files or inside
`DAT` containers (`"DAT\0"`, entry count, then offset/size pairs):

| Offset | Field | Notes |
|---|---|---|
| 0x00 | `psm` | GS pixel mode: 0x00 PSMCT32, 0x13 PSMT8, 0x14 PSMT4 (also 0x01/0x02/0x0A/0x1B/0x24/0x2C) |
| 0x01 | `state` | 0x0F while loaded; original value restored on unload |
| 0x02 | `width`, `height` | int16 each |
| 0x06 | `handle` | replaced by the CTexture image handle while loaded |
| 0x08 | `clutState` | CLUT pixel mode (0x00 = 32-bit), 0xFF = none, 0x0F = loaded |
| 0x09 | `clutPsm` | copy of the CLUT pixel mode |
| 0x0A | `clutWidth`, `clutHeight` | 8×2 = 16 entries, 16×16 = 256 entries |
| 0x0E | `clutHandle` | replaced by the palette handle while loaded |
| 0x10 | CLUT data | RGBA, alpha 0x80 = opaque, padded to 16 bytes |
| ...  | pixel data | 4 bpp low nibble first, 8 bpp, or 32 bpp RGBA; padded to 16 bytes |

The GS emulator later writes these handles into TEX0, and `CGraphic::SetTexture(handle, 0x0F,
clutHandle)` resolves them through the managed table that `CTexture` fills.

| Original (address) | Port |
|---|---|
| `Init` pre-allocates D3D texture pools per format and power-of-two size (0x40a370) | Textures created on demand |
| `Load` (0x40a800), `LoadImage` (0x40a900), `LoadClut` (0x40b300) | Same flow and state/handle bookkeeping |
| `Unload` (0x40a8a0) | Also restores the original handle values |
| `Update` (0x40b140) | Same, through `CGraphic::LockTexture` |
| Pixel conversion into a locked D3D texture (0x40b5a0, 0x40bc10) | `ps2::Decode` into CGraphic's CPU buffer |
| CLUT conversion with CSM1 reordering (0x40c5e0) | `ps2::ConvertClut` |
| A4R4G4B4 output for `texformat = 1` | BGRA8 |

Kept on purpose:
- Power-of-two texture sizes clamped to `texsize`, with texel skipping for larger images; UVs
  are normalised by the full power-of-two size, as on the PS2 and in the original.
- 16- and 24-bit images are rejected like in the original (no game data uses them).
- CLUT alpha is converted as `|(int8)a| * 255 / 128`. Game data uses 0x80 (opaque); a few
  developer test files in `cddata/test` use 0xFF, which this formula turns almost transparent,
  in the original too.
- 32-bit images keep their alpha byte unscaled, like the original.

Checked: `txm2png` decodes all 59 records of `2d/game2d_vram.dat` and the `cddata/test` TXMs;
`graphic_demo --txm` draws the same records through the GPU path with identical results.

## CGs (GS emulator)

The PC version still builds PS2 GS packets and interprets them. `CGs` replaces that layer:

| Original (address) | Port |
|---|---|
| GS register writes → D3D (0x40d1b0) | `CGs::Write` / `WriteReg` → CGraphic |
| GIF tag dispatch for VIF DIRECT data (0x413d20) | `CGs::ProcessGif` |
| PACKED (0x413900), REGLIST (0x413ab0), IMAGE (0x413c50) | same function |
| Finished image transfer (0x40e940) | `CGs::ImageDone` → `CGraphic::SetPalette` / `LoadTexture` |

What the original does, and the port reproduces:
- **Vertices** are kicked by XYZ2 only (XYZF2/XYZ3/XYZF3 are stored, never drawn). X is 12.4
  fixed point minus XYOFFSET; **Y counts field lines** (the PS2 version rendered interlaced
  fields) and is divided by 8 instead of 16. Coordinates are scaled when the resolution is not
  640×480. Depth becomes `(0xFFFFFF - Z) / 2^24`.
- **Primitives:** lines, line strips, triangle strips (nothing drawn until the 4th vertex) and
  sprites. Points, triangles and fans are ignored by the original too.
- **Textures:** TEX0 binds `TBP0` as texture handle, `CBP` as palette handle and passes `PSM` as
  the table selector: CTexture-loaded records carry 0x0F there. UV is 12.4 texels, normalised by
  the texture size; out-of-range UV bits mark screen-space coordinates. Every PRIM unbinds the
  texture.
- **Colour:** RGB stays in PS2 range (0x80 = 1.0) and is doubled by MODULATE2X; alpha is
  rescaled to 0–255.
- **Blending:** only `(Cs−Cd)·As+Cd` (normal alpha) and `Cd·FIX+Cs` (treated as additive) are
  recognised; other ALPHA settings keep the previous blend.
- **Tests:** TEST maps alpha test (function and AREF) and depth test, with the depth comparison
  flipped. Primitives that can never pass are dropped. A sprite with ZTST = ALWAYS and Z writes
  on clears the depth buffer (the game's depth-clear idiom).
- **Image transfers:** 8×2 and 16×16 32-bit uploads are CLUTs (16/256 entries) for direct slot
  DBP; other uploads become texture data at TRXPOS in direct slot DBP.

Deliberate differences:
- PRE on a PACKED tag sets PRIM once at the start of the tag (standard GS behaviour).
- IMAGE data is accepted in the same DIRECT chunk or spread over several; the original assumed it
  always arrived in the next chunk.
- Fog colour (FOGCOL) is stored but not applied: the 2D path has no fog factor.

Checked with `gs_demo`: image transfers into the direct table, REGLIST and PACKED (+PRE) sprites
with REPEAT, a triangle strip, a flat line strip, and a CTexture record referenced through
TEX0 with PSM 0x0F, all rendered from GS packets alone.

## CDma (DMA and VIF1)

The game builds one DMA chain per frame and sends it on channel 1 (0x419da0 calls the
`sceDmaSend` replacement at 0x4145b0). `CDma::Send` replaces that path:

| Original (address) | Port |
|---|---|
| `sceDmaSend` (0x4145b0), `sceDmaGetChan` (0x4138f0) | `CDma::Send(channel, base, tadr, chcr)` |
| Source-chain walker (0x414390) | same function: refe/cnt/next/ref/refs/call/ret/end, two-level call stack |
| VIF1 interpreter (0x413df0) | `CDma::Vif1` |
| DMA completion callback (0x40d180) | `CDma::SetHandler` |

- A channel-1 send is one frame: `CGraphic::BeginFrame` before the chain, `EndFrame` after
  (the original's 0x40f030 / 0x40f090 hooks).
- Only channel 1 carries data; with CHCR.TTE the tag's upper 64 bits are VIF codes, processed
  before the payload, as in the original. As there, tags with QWC = 0 skip their VIF codes.
- VIF1: STCYCL, OFFSET, BASE, ITOP, STMOD, MARK, STMASK, STROW, STCOL, MPG (skipped),
  MSCAL/MSCALF/MSCNT (double-buffer swap; MSCNT runs the VU1 handler), DIRECT/DIRECTHL
  (→ `CGs::ProcessGif`) and UNPACK (sized with the CYCLE fill rule, passed to the VU1 handler
  when WL ≤ CL, as in the original).
- **Addresses:** tag ADDR fields are byte offsets into the buffer passed to `Send`, because a
  64-bit build cannot store pointers in 31-bit DMA addresses like the 32-bit original did.

The original's UNPACK (0x40eb50) didn't fill a VU1 memory image: it remembered where each
stream lives (V2-16/V3-16 with FLG = vertex streams, V4-32 ×6 = lighting, V4-32 ×28 = camera
matrices, V4-32 at address 0 = the GIF tag/TEX0/CLAMP block) and set D3D vertex-shader
constants. MSCNT (0x40ef10) then drew the model through Vertex.cpp. That belongs to CVertex.

Checked with `dma_demo`: cnt with DIRECT in the tag, call/ret into a subroutine, ref to data
elsewhere in the buffer, end; UNPACK sizes (32 and 12 bytes) and one MSCNT per frame reach the
VU1 handler; the completion callback fires once per send.

## CVertex (3D models)

`CVertex` implements `IVu1Handler`, so the 3D path runs exactly as in the game: the DMA chain
refs a model's VIF stream, UNPACKs feed `CVertex::Unpack`, and each MSCNT calls `CVertex::Run`.

| Original (address) | Port |
|---|---|
| Register model object / model (0x411540 / 0x411600) | `RegisterObject` / `RegisterModel` |
| Build vertex buffer per packet, patch TEX0 (0x411930) | `BuildPacket` + TEX0 patch in `RegisterModel` |
| Strips (0x411e20), lists (0x4122c0) | same rules (below) |
| Vertex shaders from `cddata/vsh` (0x412710) | `vs_mesh` in `GraphicShaders.h` (all four in one function) |
| Draw strip / list model (0x412980 / 0x412b60) | `Run` → `CGraphic::DrawMesh` |
| VIF UNPACK handler (0x40eb50), MSCNT handler (0x40ef10) | `Unpack`, `Run` |
| Model header accessor (0x419a40) | `PacketBlock` |

**Model data** (`*_pdb.dat` containers, textures in the matching `*_txm.dat`):
- Model object: flags dword (bit 31 = registered), bounding box offset at +8 (8 corners), and
  up to three (offset, size) pairs at +0x10 for the models it contains.
- Model: flags byte (bit 0 = no 0x80-byte block), texture count, 6-byte texture table entries
  (texture list index, image handle, CLUT handle), then the packet block (size dword; VIF
  stream from +0x10).
- Each packet: `[MSCNT or NOP][NOP][STCYCL 4,4][UNPACK V4-32 header][UNPACK V3-16 normals]
  [UNPACK V2-16 UVs]`. The MSCNT that opens the next packet draws the previous one.
- Header: +4 flags (bits 9–11: 3 = triangle list, 4 = triangle strip; bit 12: no normals),
  +8 texture table index, +0x0C vertex buffer handle, +0x20 TEX0, +0x28 CLAMP, +0x40 GIF tag
  (NLOOP = vertex count, PRIM with PRE), +0x50 positions (x, y, z float, w = 0x8000 on the
  vertices that start a strip). Normals and UVs are int16 / 4096.

**Build rules kept from the original:**
- Strips split where vertices carry w = 0x8000; each D3D strip gets cull mode CW or CCW from
  the parity of its first vertex index, because the PS2 stream alternated winding globally.
- Lists swap the 5th and 6th vertex of every group of six and cull CW.
- Without normals, every vertex reads the first normal (as the original did).

**Constants** (camera block = UNPACK V4-32 ×28, object block = ×6):
- c1 fog (start +0x6C, end +0x7C), c2–c4 light colours (transposed matrix at +0x140),
  world→screen = matrix +0x80 × matrix +0x180 (the original wrote it back to +0xC0).
- c5–c8 local→screen = model matrix × world→screen; c9–c11 the three light directions
  (columns of the matrix at camera +0x100) in model space; c12 base colour (/128; alpha from
  ALPHA.FIX when the blend uses FIX, else from +0x4C); c13 S/T/Z offsets (Z × −1/65536).
- The lighting mode sits in the word after the object block (+0x6C): 0/3 = light (or diffuse
  for models without normals), 2/5 = stzofs, 6/7 = specular, anything else = not drawn.

**Differences:**
- The vertex buffer handle in header +0x0C is a 1-based index into CVertex's table, not a
  pointer; registration offsets stay relative (no in-place relocation).
- The world→screen matrix is kept inside CVertex instead of being written into the DMA data.
- Specular lighting is only checked to compile; no test scene uses it yet.

**Handedness:** `cull_test` confirms CGraphic culls exactly like D3D. The model data's strip
cull modes only produce outward faces with a right-handed camera (PS2 convention); the game's
camera matrices are expected to be right-handed, and `model_demo` uses one.

Checked with `model_demo`: car models from `3d/car_pdb.dat` with their textures render
correctly (body, glass, wheels) through DMA → VIF1 → UNPACK/MSCNT → CVertex → DrawMesh.
