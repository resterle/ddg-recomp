# ddg-recomp — development notes (Densha de GO! FINAL, PC, 2004)

The working notes of the recompilation: how the pieces fit together, what was found and fixed,
performance measurements, and the debugging environment. The [README](../README.md) covers
building and running. Paths are relative to the repository root; commands run in `recomp/`
unless noted. Sections 3a-3e are dated logs of the Linux, handheld and performance work (most
recent measurements: R36H, 2026-10-12, in 3e).

Like most of the code, these notes were written largely by an AI (Claude, through Claude Code)
while doing the work, as a hand-off log between sessions.

**Reached:** boot → title → menus → line/timetable select → 3D driving with HUD, keyboard and
gamepad controls, correct game speed, saves, sound and music (5a). Builds for macOS (SDL
GPU/Metal), Linux desktop (SDL GPU/Vulkan or OpenGL, 3c) and aarch64 Linux handhelds (OpenGL
ES, 3a). No movies.

**Local-only material:** the Ghidra project (`proj/`), its full decompile (`out/perfect_all.c`,
written by `scripts/ExportDecomp.java`) and the reconstructed original CGraphic header
(`src/unb/`) are game-derived and kept out of git (see `.gitignore`). References to them below
assume a local Ghidra analysis of `perfect.exe`.

---

## 1. Big picture

The PC game is a PS2 game running on a thin "PS2-on-Windows" layer written by TAITO:

```
PS2 game code (0x41d000-0x4837xx)          unchanged PS2 logic, calls sce* functions
PS2 SDK emulation (0x415000-0x41d000)      threads on Win32 events, file I/O, VU0 math, pad
PC layer "unb" (0x401000-0x415000)         CGraphic (D3D8), CTexture, GS/DMA/VIF emulation,
                                           CVertex (VU1 replacement), input (DirectInput),
                                           sound (DirectSound), movie (DirectShow)
CRT / D3DX / libjpeg (0x484000-0x4b0000)
```

We do **static recompilation**: every x86 function of the unprotected `perfect.exe` (the copy on
the CD is SafeDisc-protected; see "The exe" below) is translated to C that operates on a flat 32-bit memory image. The
Windows/DirectX boundary is replaced by:

* a Win32 shim (`recomp/host/win32.c`, ~190 imports),
* **native** re-implementations of the PC layer modules (graphics, textures, GS, DMA, VU1,
  input, movie stubs) built on **SDL3 GPU** (`port/src/unb/*.cpp`, wired in by
  `recomp/host/native.cpp`).

Game assets are *not* in the binary: `cddata/` (2.6 GB) is read from the game folder at runtime,
and `perfect.exe` is loaded at startup for its data sections (strings, tables, vtables).

**The exe** (`recomp/exepatch.py`): the input is the unprotected `perfect.exe` (md5
d1cdac65dc2afb2eb00723f2b747c52c). The CD's `perfect.exe` (061e4038…) is SafeDisc 2: sections
`stxt774`/`stxt371`, entry point in the SafeDisc loader, encrypted code; it is refused with a
message. DDGModernizer does not touch SafeDisc: it copies the (already unprotected) exe to a temp
folder, writes its patch list (`recomp/data/ddgmodernizer-final.txt`, its `Patch Configs/FINAL.txt`)
at fixed file offsets, runs the copy and deletes it. The build applies the same list in memory
with neutral values (aspect 1.0/0.5, draw distance 0.9/0.4), so a plain `perfect.exe` and a copy
DDGModernizer patched with any settings give the same image (hash 67a91e9fa9d4335f); the game
does the same when it loads the exe (`read_exe` in `host/runtime.c`, table and hash in
`build/game/exe_info.c`) and refuses an exe whose image hash differs from the build's (another
version, or the English fan translation, which adds `.trans` sections and changes code). The
player's aspect and draw distance then replace the neutral values (below).

---

## 2. Directory map

| Path | What |
|---|---|
| `out/perfect_all.c` (local only) | Ghidra decompile of perfect.exe (pre code-recovery, `scripts/ExportDecomp.java`). Best reference for "what did the original do" |
| `recomp/data/` | Function lists exported from Ghidra: `functions.tsv` (ExportDecomp), `funcs.tsv` (Survey: size, calls, callers), `reach_all.txt` (Reach). Inputs of `plan.py`/`build_game.py` |
| `proj/` (+ `proj.bak-before-recover`) | Ghidra project; `scripts/*.java` are headless scripts (ExportDecomp, RecoverCode, Survey, Boundary, …) |
| `notes/game-structure.md` | Program flow, address regions, boundary survey |
| `src/unb/Graphic.h` (local only) | Reconstructed original CGraphic header |
| `port/` | SDL3 GPU port of the PC layer + standalone demos (`port/README.md` documents CGraphic vs D3D8) |
| `port/src/unb/Graphic.cpp` / `GraphicShaders.h` | Renderer (batched draws, pipelines cache, MSL shaders) |
| `port/src/unb/shaders/scene.glsl`, `GraphicShadersSPV.h` | The same shaders for Vulkan (GLSL source, generated SPIR-V; `port/tools/build_spirv.sh`) |
| `port/src/unb/Texture.cpp` | CTexture: PS2 TXM decode (PSMCT32/24/16, PSMT8/4, CLUTs) |
| `port/src/unb/Gs.cpp` | GS register emulation (0x40d1b0): PRIM/RGBAQ/UV/XYZ kicks → CGraphic |
| `port/src/unb/Dma.cpp` | DMA source chain + VIF1 (DIRECT → GS, UNPACK/MSCNT → CVertex) |
| `port/src/unb/Vertex.cpp` | CVertex: VU1 microcode replacement, model meshes |
| `recomp/x86lift.py` | **The translator** (capstone → C) |
| `recomp/recomp_rt.h` | Runtime header for generated code (CPU struct, memory macros, x87 helpers) |
| `recomp/exepatch.py` | Reads `perfect.exe`: SafeDisc check, DDGModernizer's patches with neutral values, image hash (section 1, "The exe") |
| `docker-build.sh`, `recomp/linux/desktop/Dockerfile` | Generate and build with Docker only (desktop image: Ubuntu 22.04, SDL3, capstone; handheld: `linux/build-cross.sh`) |
| `recomp/plan.py` | Which address ranges/functions are native instead of translated |
| `recomp/build_game.py` | Discovers functions, translates, writes `build/game/*.c` |
| `recomp/verify.py`, `gen.py`, `rt_test.c` | Differential tester (Unicorn vs translated C) |
| `recomp/host/runtime.c` | Memory, PE loader, dispatch, threads/GIL, heap, path mapping |
| `recomp/host/win32.c` | Win32 API shim |
| `recomp/host/native.cpp` | Natives (WinMain, message loop, DMA send, textures, models, input, movie) + test hooks |
| `recomp/iterate.sh` | Run → on "unknown address" trap regenerate/rebuild → repeat |
| `recomp/tools/` | Scripted-run helpers (`nav.sh`, `drive_keys.sh`), screenshot tools (`magcount.py`, `montage.py`), `histo.py` (instruction histogram); see §6 |
| `recomp/build/` | Generated: `game/` (translated C), `obj/`; **`discovered.txt` is input** (function addresses found at runtime) — keep it |

---

## 3. Build and run

```sh
cd recomp
.venv/bin/python build_game.py           # regenerate build/game/ (after translator/plan changes,
                                         # new entries in build/discovered.txt or another exe)
make -j10 OPT="-O1 -DRT_CHECK_STACK"     # SDL3 from Homebrew; -DRT_CHECK_STACK = stack-balance checks
./perfect_recomp [<game folder>] [<perfect.exe>] [--settings | --play]
```

`build_game.py` reads `recomp/perfect.exe` unless `PERFECT_EXE=<path>` is set; it needs
capstone 5.0.x (`python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`).
Linux desktop (x86-64, SDL GPU on Vulkan): `linux/build-desktop.sh` (3c).

**Settings window** (`host/launcher.cpp`): shown before the game when "Show at startup" is on (the
default), when Shift is held at launch, with `--settings`, or whenever the game folder is unusable.
It sets the game folder (folder dialog), resolution, 3D aspect, draw distance, fullscreen, hiding the
screen edges and whether it appears at startup, and saves them on Start. `--play` skips it (the
test scripts use that). Drawn with SDL's 2D renderer and debug font in its own window.

**Game folder** (the install with `cddata/` and the unprotected `perfect.exe`), first match wins:
1. command-line argument,
2. environment variable `DENGO_FINAL_DIR` (and optionally `DENGO_FINAL_EXE` for the exe),
3. the folder remembered in `~/Library/Application Support/TAITO/DengoFinal/game.ini` (`gamedir=…`),
4. the settings window.

The game looks for `perfect.exe`, then `perfect2.exe` (the name earlier versions used), and takes
the first whose image matches the build. An unusable folder (no `cddata`, a missing,
SafeDisc-protected or different exe) is explained in the settings window. With `--play` the game prints the reason and exits instead.
Scripts (`iterate.sh`, `tools/nav.sh`) pass `--play`, so set `DENGO_FINAL_DIR` or pick the folder
once in the settings window before using them.

**Settings:** `~/Library/Application Support/TAITO/DengoFinal/graphics.ini` (same keys as the
original registry values). Current recommended content:

```
texformat = 1     # BGRA textures (P8 path works too, but BGRA is what was tested last)
texsize = 2       # up to 1024x1024 textures
```
Leave `screffect` at its default (7). `crop` (default 1, new) shows only the game's 600×420
canvas at 4:3; `crop = 0` shows the whole 640×480 target including the overscan margins. **Saves** (memory card emulation) go to
`~/Library/Application Support/TAITO/DengoFinal/AppData/TAITO/perfect/`.

**Enhancements** (the features of [DDGModernizer](https://github.com/hunterbridges/DDGModernizer), as
`graphics.ini` keys, applied at startup without patching anything):

| Key | Default | Effect |
|---|---|---|
| `scrwidth`, `scrheight` | 640, 480 | Render size; a wider size gives widescreen (2D UI is stretched, as with DDGModernizer) |
| `aspect` | `auto` | 3D projection X aspect; auto = (4/3 × scrheight) / scrwidth (1.0 = 4:3) |
| `aspecty` | 0.5 | 3D projection Y aspect |
| `drawdistance` | 0.6 | `RendPow` (original 0.9) |
| `okidist` | 0.4 | `OkiDistArg` |
| `fullscreen` | 0 | Borderless fullscreen at start; **F11** or **Alt+Return** toggles |
| `crop` | 1 | Show only the game's canvas (hide the overscan edges) |
| `launcher` | 1 | Show the settings window at startup |
| `texdump` | 0 | Write each new texture to `<settings>/texdump/ddg_<w>x<h>_<hash>.png` (SDL GPU, `texformat = 1`) |
| `texpack` | 1 | Replace textures by the PNGs under `<settings>/textures/` whose names end in their hash |
| `texpackmem` | 1024 | MB of decoded replacements kept for reuse (least recently used dropped first) |

**Texture packs** (`port/src/unb/TexturePack.cpp`, SDL GPU renderer only): the key is a 64-bit
hash of a texture's decoded BGRA texels and size, computed in `Flush` for each dirty texture
(`ResolveReplacement`; nothing is hashed without a dump or a pack). A replacement of another
size gets its own GPU texture (draws already recorded are re-pointed to it) with mip levels down
to the original size, generated on the GPU; a full chain made alpha-tested lattices, wires and
fences vanish at a distance. The fragment shaders sample with derivatives taken before the
per-draw branch (`gradient2d` / `textureGrad`), so the mip level is defined; the game's own
textures have one level and look as before. Dumps are encoded on a writer thread. Replacements are
decoded on demand by worker threads: `Find` queues a missing one and returns nothing, the texture is
uploaded as the original and listed in `packWaiting`, and `BeginFrame` uploads it again once
decoded. The decoded copy is dropped after the upload; an LRU cache (`texpackmem`) keeps recent
ones for scenery that is loaded again. With the full 4x pack (28,349 textures) a Yamanote Line
drive peaks at 1.5 GB (0.43 GB without; 9.2 GB when the whole pack was decoded at start-up) with
no slow ticks. `recomp/tools/texpack_bc7.py` (etcpak's BC7 encoder; its `compress_bc7` crashes
without a params object, `decompress_bc7` crashes on Python 3.14) writes BC7 DDS files (DX10
header, DXGI format 98) with the mip chain down to the original size, the last level from the
extracted original. `LoadDDS` reads them without decoding, a DDS wins over a PNG of the same hash,
and they are only used when `SDL_GPUTextureSupportsFormat` reports BC7 (Apple GPUs; lavapipe and
desktop Vulkan drivers too). The same drive peaks at 0.75 GB; pack on disk 3.0 GB (PNG 2.1 GB). When the
replacement is an exact power-of-two multiple of the original, the original texels are uploaded as
its smallest mip level, after the generated ones (`OriginalAsLastMip`): an upscaler's smoothed grain
does not reach distant scenery. `port/tools/texpack_extract` decodes every TXM record of cddata as
`CTexture::LoadImage` does and names it by the same hash (28,290 textures; 774 of the 833 dumped
in a Yamanote Line drive, the rest are composed at run time); `recomp/tools/texpack_upscale.py`
upscales them with Real-ESRGAN (line-art model for interface folders, photo model mixed 30% with
Lanczos for scenery, transparent texels filled from their neighbours first so the magenta under
them does not bleed into edges). Checked on Metal and on Vulkan (lavapipe in Docker).

DDGModernizer's patches (code fixes, aspect and draw distance sites) are applied when the exe is
loaded (see "The exe" in section 1). The ten aspect instructions read their values from a settings page
at `0x7ffc0000` instead (`plan.IMM_OVERRIDES`, emitted by the translator); the draw-distance
values are data and are written by `ApplyGameSettings` in `host/native.cpp`.

**Controls** (keyboard, game's own mapping 0x40f5e0): Return = START/confirm, Esc = back,
arrows = menu navigation; while driving ← releases the brake, → applies brake, Space = ○
(advance tutorial text). The mapping depends on the game's input mode (`0x670a60`).

Gamepads (all platforms) are mapped onto these keys in `GamepadKeys` (`host/native.cpp`):
d-pad/left stick = arrows, A/Start = Return, B/Back = Esc, X = Space, L1/R1 = PageUp/PageDown,
L2/R2 = Home/End, Start+Back quits.

### 3a. Linux / ARM handhelds (R36H, ArkOS)

`recomp/linux/build.sh` builds `build/arm64/ports/` and `build/ddg-recomp-arm64.zip`: copy the
contents to the SD card's `ports` folder, then put `cddata/` and `perfect.exe` into
`ports/ddg-recomp/gamedata/` (see `linux/README.txt`, which ships in the package).
On an x86-64 Linux host use `recomp/linux/build-cross.sh` instead (same output): it compiles with
clang 16 in an x86-64 container (`linux/cross/Dockerfile`) against a sysroot exported once from the
`ddg-recomp-arm64` image (`build/sysroot-arm64/`, absolute symlinks made relative). Building the game
inside the arm64 image would run under qemu; the cross build takes about a minute.

- **Toolchain:** Docker image `ddg-recomp-arm64` (`linux/Dockerfile`), Debian buster arm64 so the
  binary runs on old glibc (needs 2.17+; ArkOS has 2.30), clang 16 from apt.llvm.org, and SDL
  3.4.16 built with KMSDRM + GLES only; EGL/GBM/DRM are loaded at run time from the device's Mali
  driver. On Apple Silicon the container runs natively.
- **Renderer:** `RENDERER=gl` selects `port/src/unb/GraphicGL.cpp` (OpenGL ES 3.0; desktop GL
  3.3 core on macOS for testing: `make RENDERER=gl` → `perfect_recomp_gl`). It implements
  the same `CGraphic` interface. The file header lists the differences from `Graphic.cpp`:
  context per thread, no depth clamp, render targets are FBOs, BGRA uploaded via swizzle.
  The RK3326 (Mali-G31) has no Vulkan, so SDL GPU is not an option there.
- **Packaging:** `linux/package.sh` builds the folder (`ports/ddg-recomp/`, until 2026-10-12
  `ports/densha/`; the launcher moves an old install's gamedata and conf over on its first start). `linux/ddg-recomp.sh` is the PortMaster-style
  launcher. It uses PortMaster's `control.txt` for the controller mapping when present, sets the
  CPU/GPU governors to performance, and sets `DENGO_FINAL_DIR`, `RECOMP_PREF_DIR=ddg-recomp/conf`
  and `LD_LIBRARY_PATH`. Handheld defaults are in `linux/graphics.ini` (32-bit textures: the P8 shader path, palette lookup and manual filtering, made the Mali-G31 take 44 ms per menu frame; `texsize 1`; `frameskip 3`).
- **Where the R36H runs it from:** with ArkOS on two SD cards, EmulationStation's Ports list is
  `/roms/ports/` (`/etc/emulationstation/es_systems.cfg`, exFAT partition of the system card),
  not `/roms2/ports/`. Install there; a copy in `/roms2/ports` is not what the menu starts.
- **Portability bits:** the guest memory base is 0x400000000 on Linux (fits 39-bit address
  spaces), and the guest memory is a memfd mapped twice (`host/guestmem.h`).
  `rt_fix_case` resolves guest paths case-insensitively (`host/runtime.c`).
- **Speed:** each message-loop tick runs one whole game frame, so a device that cannot draw at
  60 fps would run the game in slow motion. `frameskip = N` (default 3 in the handheld ini) skips
  drawing in ticks that start more than one period late (`N_004075a0`, using the loop's
  schedule), so game logic keeps 60 Hz and the frame rate drops instead. The arm64 build is tuned
  with `-mcpu=cortex-a35`. `RECOMP_TRACE_TICKS=1` (on in `ddg-recomp.sh`) logs every 10 s:
  ticks/s, game/render ms per tick, skipped frames, and GL flush/swap times (`gl:` lines).
  `RECOMP_VSYNC=0` turns off waiting for the display.
- **GL draw path (GraphicGL.cpp):** per-draw parameters live in one std140 uniform block per
  draw; all blocks of a flush are uploaded at once and selected with `glBindBufferRange` (one
  call instead of ~9 `glUniform*`). Shader variants are compiled at start-up per mesh/flat/
  texture mode/alpha test/fog (`ProgramKey`). Fixed-function state is shadowed (`ApplyState`),
  and adjacent mesh draws with identical parameters are merged. On the R36H (before these
  changes) a driving frame cost ~37 ms of GL submission and ~55 ms of GPU time; in Docker the
  changes took driving from ~42 drawn fps to 60.
- **Batching (`batching = 1`, GL only):** mesh draws are not drawn one by one. In `Flush`, runs
  of order-independent draws (depth test and writes on, opaque or alpha-tested over-blending,
  ~87% of model draws) are stably sorted by state/texture/fragment parameters. Their raw vertices
  are copied into one stream buffer per flush, and equal neighbours become one `glDrawArrays`.
  Each batch binds a window of up to 255 draws' vertex constants (13 vec4 each, `Draws` uniform
  block, sized from `GL_MAX_UNIFORM_BLOCK_SIZE`); a per-vertex slot byte selects them
  (`kVSMeshBatch`). Meshes destroyed before the flush are kept until it (`deadMeshes`). Driving:
  ~1,230 draw calls per frame → ~410 (~330 at draw distance 0.4).
- **`fastalpha = 1`:** at `UnlockTexture` the BGRA staging is scanned for alpha < 255
  (`AnalyzeAlpha`). A mesh draw with SRCALPHA/INVSRCALPHA blending and an alpha test >= ref whose
  texture has no texel alpha in [ref, 254] (and vertex alpha 1) drops the blend; with no
  transparent texels at all it also drops the test. Mali can then reject hidden pixels early.
  Driving: blended draws 398 → 187, alpha-tested 219 → 77.
- **`mipmaps = 1`:** `glGenerateMipmap` for fully opaque BGRA textures only (alpha-tested foliage
  would thin out); samplers use `*_MIPMAP_NEAREST`.
- **Depth:** cleared for real at `BeginFrame` and invalidated before present (no tile loads or
  stores of depth on Mali).
- **Cheaper recording:** consecutive strips of one model with identical constants and state are
  appended to the previous draw (`lastStrip`, strip ranges in `stripRuns`); sorting uses a
  precomputed (pipeline key, hash) per draw, with an exact comparison before merging.
- **Guest memory on macOS:** `rt_reserve_guest_memory()` runs first thing in `main`. Apple's GPU
  driver reserves ~12 GB at a random address once a window exists. The base is 0x10000000000:
  before `main`, malloc reserves a ~12 GB region at a random place in 0x7000000000–0x8000000000,
  which covered the former base 0x8000000000 (alias from 0x7f00000000) in about one launch in ten
  ("guest memory: mach_vm_allocate at 7f00000000 failed").
- **VU0 intrinsics:** 0x415740/0x415760/0x415700/0x415810 are expanded inline by the translator
  (`x86lift.INTRINSICS`, helpers `rt_vu0_apply`/`rt_vu0_mul` in `host/d3dx.cpp`) instead of going
  through the native boundary. They are called from the game's inner loops.
- **Callee-saved registers (`x86lift.preserve_mask`, `LIFT_NO_PRESERVE=1` to disable):** a
  translated function that provably returns EBX/EBP/ESI/EDI unchanged does not write them back
  to `R`, and callers keep their copies (`RT_RETM`/`RT_CALLM`, masks in the generated code).
  "Provably": saved in the prologue and popped before every exit, or never written while all
  direct callees preserve it; tail-call targets must preserve it either way. Two rules keep `R`
  correct for code that reads it: adapters `A_x` write the preserved registers' entry values back
  (`Lifter.adapter`), and a tail call into a function preserving more than the caller stores
  those registers after the call (`tail_stmt`). ~37,000 calls per tick; game logic -12%
  (4.95 → 4.38 M instructions per tick). Differential tests: 988 pass; the 2 failures are loops
  that never end with random inputs (they hang without this change too). `verify.py` now gives
  each function 60 s.
- **PGO (`linux/pgo.sh`):** builds an instrumented arm64 binary (`PGO_MODE=gen`,
  `-DRECOMP_PGO_GEN`), plays ~130 s of game time in Docker (`RECOMP_EXIT_AFTER` writes the profile
  and quits), merges `linux/perfect.profdata`; `build.sh` uses it automatically when present.
  Re-run after larger code changes. No difference measurable on Apple cores (large out-of-order
  cores hide layout effects); expected to matter more on the in-order A35.
- **macOS test runs:** with vsync on, SDL's Cocoa GL swap waits for the display's refresh; when
  the display sleeps or the window is hidden the game thread blocks there and the PS2 thread
  emulation deadlocks with the vsync tick. Unattended runs need `RECOMP_VSYNC=0`.
- **Measuring without the device:** `RECOMP_TRACE_TICKS=1` adds `gl:` lines with draw calls,
  blended/alpha-tested counts and (macOS) instructions per frame for list building, upload and
  submission. `RECOMP_GL_NOSUBMIT=1` skips the GL draw calls, which isolates our own CPU cost
  from the driver's. Instruction counts on Apple Silicon are a fair proxy for the A35 (same ISA;
  ~1G instructions/s there, so ~16M per 60 Hz tick).
- **Cull direction (fixed 2026-10-10):** the GL renderer had D3DCULL_CCW/CW backwards, so every
  culled model (buildings, platforms, ballast, the sky dome) was dropped while unculled ones
  (roof, masts, rails) drew; this was the "missing surfaces" on the R36H, and it reproduced on
  macOS with `RENDERER=gl` all along. With `glFrontFace(GL_CCW)`, D3DCULL_CCW is `GL_FRONT`.
  Compare GL screenshots against the SDL GPU renderer (or `build/drive_43.png`), not only
  against earlier GL shots. `RECOMP_GL_DEBUG` (bits: 1 no culling, 2 no depth test, 4 opposite
  cull direction) isolates such bugs.
- **Render thread (`threaded = 1`, default):** `Flush()` is split into `Capture()` (game thread,
  under `g->lock`: takes the recorded draws, copies dirty texture/palette data, releases arena
  ranges) and `Execute()` (all GL work). With threading the jobs queue for a render thread that
  owns the GL context and also presents; `EndFrame` keeps one frame in flight, and the frame
  limiter (`N_004075a0`) skips drawing while `CGraphic::FrameInFlight()` (within `frameskip`),
  so the game thread never waits for the driver or the GPU. Meshes are immutable and freed only
  after the jobs that may read them (`deadMeshes`). `threaded = 0` / `RECOMP_GL_THREADED=0` runs
  Capture+Execute back to back (the old behaviour). `RECOMP_GL_SLOW=<ms>` delays each frame on
  the render thread to imitate a slow GPU on the desktop.
- **Scenery changes (stutter, fixed 2026-10-11):** every ~30 s of driving the game frees ~500-700
  textures and ~3000 models and loads new ones in one tick. `DestroyTexture` used to flush and
  scan both 16K slot tables per call and `DestroyMesh` erased from a vector: 15 ms of a 18 ms tick
  on the Mac, ~200 ms on the R36H. Textures now remember their slots (`texSlot`, `managedSlots`),
  meshes their index (swap-remove), and destroying no longer flushes. On the render thread the
  new textures' uploads (most are drawn at once) can take long; the frame limiter then skips up
  frames while `FrameInFlight()` (no cap: only this limiter reads the skip counter 0x670ad8)
  instead of waiting. On the R36H the 238 uploads of one scenery change took 372 ms, so: uploads
  not needed by a job are spread out (`UploadTextures`), mipmaps are generated afterwards (~2 ms
  per job, `GenerateMipmaps`; level 0 is drawn meanwhile), GL texture deletes go 64 per job,
  storage is allocated with `glTexStorage2D` (all levels at once), and no `glGetError` per
  texture. `tex16 = 1` stores opaque textures as RGB565 (half the upload data; off by default).
  The slow-GL-job log line splits upload time into create / data / mipmaps. `RECOMP_TRACE_SPIKES=<ms>` (on in `ddg-recomp.sh`, 40) breaks down
  slow ticks (resource calls, file reads, draw submit, waits) and slow GL jobs.
- **Frame pacing with the render thread:** a frame takes the R36H's render thread ~37 ms (~12 ms
  driver CPU for ~300 draws + ~25 ms swap). Starting a frame only when the previous one was done
  quantised that to every third tick (50 ms, 20 fps). The limiter now draws when
  `CGraphic::FrameBusyNs()` (hand-over time + moving average of frame durations) says the render
  thread will be done within 10 ms (`RECOMP_GL_AHEAD_MS`); the game thread waits that long at
  `EndFrame`. Simulated 35 ms frames: 20 -> 25.8 fps at 60 ticks/s. `RECOMP_TRACE_TICKS` now also
  samples GPU time (glFinish before every 30th swap) to split "swap" into GPU and display wait.
- **Slow frames and the texture pool (2026-10-11, device log):** a scenery change's render job
  took 358 ms on the R36H (238 uploads: create 218 ms, data 109 ms), and `FrameBusyNs` treated
  the overdue frame as "done any moment", so the game started a frame and waited ~340 ms. Now an
  overdue frame (>10 ms past the estimate) counts as busy, two frames may be in flight (EndFrame
  waits only when two are queued; per-frame hand-over times in `handoverNs`), and freed GL
  textures go to `texPool` (by size/format/levels, 64 MB cap) and are reused by later creations,
  already before the uploads of the same job when the job's draws do not use them. Mac: all 136
  textures of the first scenery change reused. `RECOMP_GL_SLOW_ONCE=<s>:<ms>` delays one frame
  (tests the pacing; a 350 ms frame no longer stalls the game).
- **FPS counter:** `fps = 1` (graphics.ini, GL renderer) draws frames presented per second (half-second
  windows) in the top-left corner: `DrawFpsCounter` renders a 3x5-pixel font into a small texture
  and draws it with the present shader after the scene. It counts drawn frames, not game ticks
  (the game still runs at 60 ticks/s when frames are skipped).
- **Crash reports:** `rt_install_crash_handler()` (start of `main`) prints signal, fault address
  (also as a guest address), the host backtrace and the guest registers to stderr / `log.txt`.
  Resolve the `perfect_recomp(+0x…)` offsets with `llvm-symbolizer --obj=<binary>` against the
  same build (copies of shipped binaries: `recomp/build/released/`). The launcher also logs
  `free -m` and the kernel log tail after a non-zero exit (OOM kills).
- **Device status (R36H, ArkOS, Mali-G31 r13p0) before 3b:** ~50-60 ticks/s while driving with
  frameskip 3 (about 14 drawn frames/s); `densha/log.txt` on the card has the run log.

### 3b. GL backend, second round (2026-10-12): asynchronous present, fewer and cheaper draws

Measured on the R36H with `RECOMP_TRACE_TICKS` (Yamanote Line, same scripted run, same start
temperature): 20-23 frames/s before, 56-60 shown after, standing and driving (table below). Where the time went before: the
render thread spent ~14 ms per frame issuing GL (~9 ms of it ~350 draw calls at ~26 µs of Mali
driver time each) and then ~25 ms in `SDL_GL_SwapWindow`: SDL's KMSDRM backend does a blocking
atomic commit (no `EGL_ANDROID_native_fence_sync` on this driver), which returns after the GPU
has finished the frame *and* the next vblank, so CPU and GPU never overlapped.

- **Asynchronous present** (`PresentMain`, `HandOff`, `AcquireTarget`): three contexts in one share
  group. The render thread draws with `renderCtx` made current without a surface
  (`EGL_KHR_surfaceless_context`), puts a fence after the scene and hands the scene texture to the
  present thread (mailbox of one), which owns the window's context, queues the blit behind the
  fence (`glWaitSync`) and blocks in the swap. The scene render targets alternate every frame
  (`tex[0]`/`tex[1]`, as with the screen effect). Rules: a target is not drawn into while the
  present thread blits from it (and the GPU waits for that blit's fence); the render thread waits
  in `HandOff` until the previous scene's blit is queued before flushing the next scene (otherwise
  the next scene's GPU work went ahead of the blit, swaps missed vblanks and finished frames were
  dropped), and at most one frame is queued behind the GPU (`frameFence`). Falls back to
  presenting on the render thread when a surfaceless context is not really current
  (`MakeCurrentSurfaceless`: SDL's offscreen driver reports success without one).
  `RECOMP_GL_ASYNC_PRESENT=0` disables it. On the device the `vsync` setting no longer matters:
  the commit always waits for the vblank, and there is no tearing.
- **Shader variants** (`ProgramKey`): the fragment shader is compiled per colour op, alpha op,
  alpha-test function and v-flip instead of branching on uniforms per draw (the trivial-shader
  experiment showed fragment shading as ~8.5 of ~18.7 ms GPU time); colour math in mediump.
  Variants are made on first use (~130 ms each on the Mali), stored as program binaries in
  `shaders.bin` in the settings folder (keyed by driver and shader source hash) and loaded from
  there on later runs (14 variants in ~11 ms). `kWarmKeys` (the 13 variants seen in menus and
  driving) are compiled by a warm-up thread with its own context while the game starts, so the
  first run has no compile stalls either.
- **Batched draws** carry everything their shaders read in their draw slot (fog colour and alpha
  reference in `lightLocal[1..2].w`), share one slot window per 78 draws (one `BindBufferRange`
  per window instead of per draw) and sort by program first: per frame ~15 window binds instead of
  ~220, ~80 parameter-block binds instead of ~250, ~85 program binds instead of ~145.
- **Indexed models** (`BuildIndexed`): CVertex hands over unrolled triangle lists; the render thread
  removes repeated vertices once per model (~40% fewer vertices) and draws batches with
  `glDrawRangeElements` (plain `glDrawElements` made the driver scan the indices: +6 ms per frame).
  Indexing is budgeted (2 ms per job), so thousands of new models at a scenery change are indexed
  over a few frames and drawn unindexed meanwhile.
- **Texture arrays** (`TexArray`, `texarrays = 1`, default off): BGRA textures as layers of
  per-size-class `GL_TEXTURE_2D_ARRAY`s, layer per draw; draws then merge across textures (draw
  calls 333 -> ~150-220, submit 10.7 -> 6 ms) and creating a texture takes a free layer instead of
  allocating storage. But the Mali-G31 samples arrays clearly slower (even one-layer arrays, with
  driver-made mipmaps too; probably no AFBC for arrays), which cost more GPU time than the CPU it
  saved, so it is off. Kept for other GPUs.
- **Scenery changes** (`UploadReq`, `ApplyUploads`): a scenery change brings ~136 new textures
  that the very next frame draws, and the uploads (~0.6 ms each in the Mali driver, plus ~0.65 ms
  allocation for a texture not in `texPool`) held that frame up: the picture stood still for
  190-220 ms. Now (threaded, without texture arrays):
  - `UnlockTexture` queues the new content for the render thread at once (`IssueUpload`), and the
    render thread uploads queued textures whenever no job is waiting (`RenderMain`): it is idle
    while the game thread computes the loading tick (~40-55 ms), so about a third are done before
    the frame that needs them arrives.
  - Model textures that never had content are uploaded within 3 ms per 1/60 s of frame time
    (6 ms per frame at 30 frames/s; `RECOMP_GL_UPLOAD_BUDGET=<ms>`, 0 = always wait), with their
    mipmaps at once; the rest are drawn for a few frames with a stand-in (`MakeStandIn`): an 8x8
    thumbnail when the texture is opaque, transparent when it has transparent texels (cut-out
    trees and signs appear a few frames late). 2D draws and re-uploads of textures that already
    have content always wait. 256 stand-ins are made ahead in idle time (creating a texture costs
    ~0.45 ms). Requests are kept in order per texture; those of textures freed meanwhile are
    dropped. `standins = 0` (graphics.ini) makes model textures always wait;
    `RECOMP_GL_EARLY_UPLOADS=0` turns all of this off.
  - First version (2026-10-12, reported as textures "flashing" on the R36H at 30 frames/s and
    draw distance 0.90, where a scenery change made up to 169 stand-ins): 1x1 stand-ins with the
    average colour, transparent ones for cut-outs, mipmaps a few frames after the upload. Tried
    16x16 thumbnails: filling 168 of them cost 86 ms on the Mali driver against 3 ms at 8x8 (the
    driver's tiled layout starts at 16x16; committing the storage ahead did not help). Seen on
    screenshots across a scenery change: the half-transparent copies of poles and buildings are the
    game's motion blur (screen effect), not stand-ins.
  - Result at the route's scenery change (t=107 s of the benchmark): render job 116-137 ms ->
    30 ms, longest time without a new frame 192 -> 102-115 ms; the rest is the game's own loading
    tick (56 ms: ~39 ms game logic, ~14 ms creating/freeing textures and models) plus one frame.
    Screenshots during and after the change show no stand-in colours; 140 s later the frame is
    identical to the old build's.
  - Tried and dropped: an upload thread with its own context in the share group. The Mali driver
    serialises the contexts: uploads there took 1-7 ms each instead of ~0.6, and the render thread
    then waited for them (one 640 ms gap).
- **30 frames/s mode** (`maxfps = 30` in graphics.ini, `N_004075a0`): draws only every second
  tick (20, 15: every third, fourth); a due frame skipped for being late is drawn on the next
  tick. Driving on the R36H (frames shown per second, 10 s windows): draw distance 0.90 ->
  29.7-30.0 (render thread ~17-22 ms per frame of 33), 0.70 -> 29.8-30.0. At 60 the same route
  gives 0.40 -> 58.7-60, 0.50 -> 54.9-59.9, 0.60 -> 49-59, 0.90 -> 32-45 (render-thread CPU:
  draw calls 250 -> 400-580, driver cost per draw).
- **Display:** the R36H's panel mode is `640x480p78` (one mode only), i.e. a refresh every
  12.8 ms; the game's 60 frames per second are shown for one or two refreshes each (gaps of 12.8
  and 25.6 ms in the `RECOMP_TRACE_SPIKES=15` log). The `gl:` lines now also report the longest
  gap between shown frames and the gaps over 50 ms; `RECOMP_TRACE_SPIKES=<ms>` logs every gap
  longer than that.
- Tried and dropped: mediump alone (no change), uploads through a pixel unpack buffer (the copy
  just moves to the next GL call), legacy KMS page flips (`SDL_KMSDRM_ATOMIC=0`: flips fail with
  EINVAL and frames are lost).
- **Diagnostics**: the `gl:` lines of `RECOMP_TRACE_TICKS` now split the render thread's time
  (textures, list, upload, submit, display wait, gpu wait), count frames shown and dropped, the
  present thread's time per present, and binds per frame. `RECOMP_GL_EXP` = 1 (trivial fragment
  shader), 2 (every draw scissored to one pixel: vertex/tiling cost), 4 (no present blit) split
  the GPU time. GPU utilisation: `/sys/class/misc/mali0/device/utilisation`.
- **Measuring on the device**: stop EmulationStation (it holds the display), run with a copy of
  the save data (an empty save folder hangs at start on Linux), `RECOMP_KEYS` as in
  `tools/drive_keys.sh` (with a save the driving scene comes at ~40 s; to drive off, left arrow
  x14 releases the brake, then down arrow x5 is power 5). The R36H throttles the CPU to
  1008 MHz above ~70 °C after a few minutes of load: compare builds only at the same starting
  temperature (`/sys/class/thermal/thermal_zone0/temp`) and log `scaling_cur_freq`.

| R36H, frames shown per second (10 s windows) | standing | driving |
|---|---|---|
| before (installed build of 2026-10-11) | 19.5-20.3 | 20.1-22.6 |
| + asynchronous present, specialised shaders | 52-57 | |
| + slot windows, fragment parameters in slots, program sort | 54-58 | |
| + indexed models, shader warm-up and cache (final A/B) | 55.8-60.0 | 56.1-59.9 |

The final A/B ran with the CPU throttled (1512 MHz only 3% of the time, the old build 69-100%:
it needs less CPU); GPU utilisation 67% (old: 35-40%). Remaining per frame on the render thread:
~3.5 ms list building, ~0.9 ms upload, ~7-9 ms submission, of which most is the Mali driver
(`RECOMP_PROFILE`: 61-66% of the render thread's samples are in libmali).
- The main thread's tick wait sleeps once (`DelayTick`) instead of `SDL_DelayPrecise`'s 1 ms steps
  plus spinning: its share of the process CPU went from 12% to 8.5%.

---

### 3c. Linux desktop with Vulkan (2026-10-12)

`recomp/linux/build-desktop.sh` builds `build/linux-x64/perfect_recomp` with the SDL GPU renderer
(`Graphic.cpp`) and the system's clang and SDL3, bundles `libSDL3.so.0` (rpath `$ORIGIN/libs`)
and packs `build/ddg-recomp-linux-x64.tar.gz` (`linux/README-desktop.txt` inside). `RENDERER=gl`
builds the GL backend instead.

- **Shaders:** `port/src/unb/shaders/scene.glsl` is the Vulkan GLSL version of the MSL in
  `GraphicShaders.h` (scene 2D, models, present; flat-colour variants). `port/tools/build_spirv.sh`
  compiles the ten variants with `glslangValidator` (and checks them with `spirv-val`) into the
  checked-in `GraphicShadersSPV.h`, so building needs no shader compiler. Change MSL and GLSL
  together. `MakeShader` takes MSL when the device accepts it, else SPIR-V (entry `main`); the
  `RECOMP_DEBUG_MESH_*` shader variants exist only for Metal. Resource sets follow SDL's SPIR-V
  rules (vertex: set 0 storage buffers, set 1 uniforms; fragment: set 2 samplers then storage
  buffers, set 3 uniforms). SDL flips the Vulkan viewport, so clip space is y-up as on Metal.
- **Render pass order (bug fix for all SDL GPU backends):** `Flush` drew all 3D draws of a flush
  in one pass and then all 2D draws. The game clears the 3D background with a 2D rectangle drawn
  first (depth test "always", z = 1), which then covered the whole scene (seen in the driving
  tutorial on Vulkan: only HUD and tutorial graphics on grey). Passes now follow the draw order
  (one per run of 2D or 3D draws, the first 3D pass with a cleared depth buffer, as before).
  Not re-checked on Metal.
- **Untranslated functions** (traps "call to unknown address"): Conductor Mode hit 0x434700,
  Osaka Loop diagram 02 hit 0x437c40. Both are only referenced from pointer tables in `.data`,
  and the byte before them is 0xff (the end of a switch table), so `build_game.py`'s rule for
  pointers to code outside Ghidra's functions (preceded by padding or `ret`) skipped them. A scan of
  all such pointers (data and code immediates) that are not inside a known function found two more
  plausible entries, 0x41d630 and 0x478650; all four are in `build/discovered.txt`, and the
  regenerated code differs only by these functions. Osaka Loop diagram 02 then played ~3 minutes
  (rain, ATS stop, overrun) without a trap. Note: the runtime appends to `build/discovered.txt`
  relative to the working directory, so a trap in a run outside `recomp/` lands elsewhere.
- Checked on an AMD Renoir (RADV): menus, driving tutorial and Conductor Mode at 1280x960 with
  draw distance 0.9, 60 frames/s; screenshots match the GL backend's. `RECOMP_GPU_DEBUG=1`
  creates the device in SDL's debug mode.

### 3d. The PS2 packet path on the game side (2026-10-12)

Every frame the game builds a PS2 DMA chain; a guest thread of its own walks it in `CDma::Send`
(VIF codes, `CGs::ProcessGif` for 2D, `CVertex::Unpack`/`Run` -> `DrawMesh` for models). Guest
threads run one at a time (the runtime's lock), so this time adds to the game logic's. On the R36H
at draw distance 0.90 a frame's chain is ~3200 DMA tags, ~31000 VIF codes (half NOP padding) and
~2700 model packets; `dma:` lines (`RECOMP_TRACE_TICKS`) give its time per frame (chain start to
`EndFrame`). Profile of that thread: no single hot spot, but per-packet work dominated by cache
misses on the first touch of each packet's data (tags, VIF codes, header, model table, mesh)
and by lock/compare overhead per draw. Changes, measured on the same route (ms per frame):

| | ms per frame (4 windows of 10 s) |
|---|---|
| before | 10.10 9.05 9.46 8.57 |
| + one `DrawMeshStrips` call per strip model (one lock and one state comparison instead of one per strip), material writes skipped when a packet repeats the previous one's (`CGs::WriteSerial`), NOPs skipped two words at a time | 9.67 9.08 8.62 8.42 |
| + prefetching in the chain walker (tags ahead, the next tag's data) and of the model entry | 9.89 8.20 8.53 7.79 |
| + `RecLock` instead of `std::recursive_mutex`, held across the chain (`HoldRecordingLock`; released before `EndFrame`, which may wait) | 8.51 7.28 7.15 7.21 |

About 20% less (~1.9 ms per drawn frame). Also: `DrawMeshStrips` does not read the mesh object at
all with batching (one cache miss less per model; not measured separately).
Profiler: samples now carry the link register; `tools/profile.py` with `PROFILE_CALLERS=1` names
the callers of library code (memcmp, mutexes), `PROFILE_FUNCS=1` lists translated functions by
name. Note: ITIMER_PROF samples count kernel time in syscalls (futex) at the syscall's PC.
Benchmark runs on the device drive scripted input; a person using the device at the same time
(or the controller) sends real input too, and the script goes astray (all-menu runs).

### 3e. Guest thread wake-ups and the game's math (2026-10-12, measured on the PC)

- **Wake-ups:** the runtime had one condition variable for all waits (events, semaphores,
  critical sections, thread messages, suspended and finishing threads) and woke every waiting
  guest thread on each `SetEvent`/`LeaveCriticalSection`/... (`pthread_cond_broadcast`). Each
  woken thread took the runtime lock only to go back to sleep, while the running thread waited
  for it: about half of the guest threads' CPU time on the PC was futex calls. Now every thread
  sleeps on its own condition variable and records what it waits for (`rt_wait_objs`), and
  `rt_signal_obj(obj)` wakes only those threads (`rt_signal()`/`rt_wait()` remain as wake-all /
  wait-for-anything). PC, Osaka Loop drive: game 2.3 -> 1.6 ms per tick, process CPU 7.4 ->
  4.2 ms per tick; the route ends on the identical frame. R36H: see the last item.
- **sin/cos:** the rotation natives call `sincos` once instead of `sin` and `cos` (glibc uses the
  same kernels: identical float results for 10^8 angles on x86-64 and on the R36H's glibc 2.41). Game 1.60 -> 1.54 ms per tick on the PC, identical frames.
- Where the game threads spend their time now (PC): the scene thread in the per-object loops
  (`F_0047c8c0`, `F_0047be80`, `F_0047c4b0`: transform, distance, visibility) and
  `Vec3Transform` (one call per object, already vectorised and kept bit-exact with the x87
  results); a third thread in matrix products (`Multiply`, called very often) and
  `F_0041ee70`/`F_00415920`/`F_004206f0`. Further gains need fewer calls (native versions of
  those loops) or a leaner translation, not faster helpers.
- **Scene loop, measured (PC, Osaka Loop drive):** the object loop `F_0047c8c0` (~44 calls and
  ~375 objects per tick) takes ~0.29 ms per tick including its callees, ~19% of the game's
  ~1.55 ms; the track loop `F_0047c810` ~0.08 ms. The time is spread over a dozen callees
  (per second: draw queueing `F_00466b50` ~4 ms, `F_0047c8a0` ~3.7, visibility `F_00473c40`
  ~3.3, `F_00466ac0` (a float getter, 35000 calls) ~2.9, map box test `F_00473e00` ~2.5,
  fade/LOD `F_0047cc50` ~1.7, the vector transform ~1.6; inflated by the timing). Inlining the
  small x87 leaves (getters, vector subtraction; `LIFT_INLINE_X87=1`) gives identical frames
  but no measurable gain on the PC, so it is off. A native loop pays off only together with
  native versions of the larger callees.
- **The exe on the PC changed (2026-10-12 16:44, outside the recompiled game):** an English fan
  translation patch (`.trans` sections) was applied to the `perfect2.exe` used until then. The
  translated code is generated from the exe it will run with: regenerating gave different code
  (7 functions) and different frames. Since then the build takes the plain `perfect.exe` and
  records the image hash, and the game checks it (section 1, "The exe"); the R36H's
  `gamedata/perfect2.exe` is that translated exe, so a build made from `perfect.exe` needs
  `perfect.exe` on the device. Checked 2026-10-12: the device's exe has the same
  md5 as the PC's (6b8f169380a7246209db0c714fbf1713).
- **R36H, 2026-10-12** (Yamanote drive, 150 s, maxfps 30, two alternating runs per build, values
  t=60-150 s): installed packet-path build vs this build (wake-ups, `sincos`, dispatch fix):
  game 9.4 -> 7.2 ms per tick, process CPU 27 -> 19 ms per tick, packet chain 6.2 -> 3.3 ms per
  frame, GPU load 47 -> 32%; frames over 50 ms per 10 s 11-26 -> 3-11, longest gap 89-206 ->
  51-141 ms. Both builds already show ~29.8 of 30 frames/s at this point, so the gain is
  headroom (draw distance, `maxfps` 0, busier scenes). Screenshots differ between builds no more
  than between two runs of the same build (frame skipping shifts the captured frame).
  `sincos` vs `sin`/`cos` on the device's glibc 2.41: 0 of 99.8 million float results differ.
  Installed in /roms/ports/densha and /roms2/ports/densha (`perfect_recomp.prev4-20261012` is
  the previous binary). Device bench folder: /roms2/dbench (`dbench.sh`, save, graphics.ini).

## 4. How the recompiler works (essentials)

* **Translation** (`x86lift.py`): one C function `F_xxxxxxxx` per x86 function. Registers are
  locals, saved to/loaded from the global `R` around calls (`SAVE()`/`LOAD()`). Flags are locals.
  Memory is `MEM + addr` (`RD32`/`WR32` …). x87 stack lives in `R.st[]` (double precision).
  Jump tables become `switch` on the loaded target. Indirect calls/jumps go through
  `RT_CallAddr(addr)` (binary search in `dispatch.c`).
* **Imports** get fake addresses `0xfe000000 + 4*i`; host callbacks for COM vtables use
  `rt_host_fn()` → `0xfd000000 + 4*i`.
* **Natives**: calls into ranges listed in `plan.py` (`NATIVE`, `NATIVE_FUNCS`) are routed to
  `N_xxxxxxxx` (implemented in `host/native.cpp`; missing ones get weak stubs in
  `build/game/native_info.c` — Movie/Input stubs return 1, others trap).
* **Function discovery**: roots + pointer scans + heuristics; at runtime an unknown target is
  appended to `build/discovered.txt` and the program traps; `iterate.sh` (or `tools/nav.sh`)
  regenerates and rebuilds.
* **Threads**: real host threads with a global lock (GIL); per-thread CPU state and TIB.
* **Memory layout**: image 0x400000–0x857fff, heap 0x10000000+, VirtualAlloc 0x40000000+,
  stacks from 0x60000000 (1 MB each).
* **Verification**: `verify.py` runs functions in Unicorn and in translated C and compares
  memory/registers; 539 leaf + 987 game-only call trees passed with 0 failures.

Current output: ~3,987 functions translated, 118 failed (MMX/SSE/3DNow! in D3DX math —
not reached so far), 69 natives.

---

## 5. Work log — what was found and fixed (this phase)

Recompiler / runtime:
1. **Indirect call target evaluated after pushing the return address** → wrong target for
   `call [esp+x]`. Fixed in `emit_call`.
2. **Functions with code located before their entry point** (shared exit blocks, e.g.
   `strchr` 0x4a3880 jumping to 0x4a3870) started executing at the first emitted block.
   Fix: emit `goto L_<entry>` first (`x86lift.py`, `function()`). Affected 10 functions; this
   broke string handling and indirectly hid the HUD.
3. `QueryPerformanceCounter` returned host uptime (~1.9e6 s); the game's frame limiter keeps
   time in a float (0.125 s resolution at that magnitude) → bursty rendering/stutter. Now
   counts from process start (`win32.c`).
4. `SHGetSpecialFolderLocation`/`SHGetPathFromIDListA`/`SHGetMalloc` implemented (CSIDL_APPDATA
   → `C:\AppData` → pref dir). Without it the memory-card path was empty and saving failed.
   Added `rt_host_fn()` for COM vtables. Path mapping order fixed in `rt_map_path`.
5. Makefile: header dependency tracking (shader/header-only edits were silently not rebuilt).

Natives (`host/native.cpp`):
6. `sceGsSwapDBuffDc` (0x414c60) native: the draw environment (FRAME, ZBUF, XYOFFSET, SCISSOR,
   TEST…) is written there into the original GS globals, not via DMA → forwarded to `CGs`.
   Without it XYOFFSET was 0 and everything was drawn off-screen (black screen).
7. Movie natives (0x4066f0/0x406710/0x4068d0): report "done, no movie" → movies skipped
   (the game spun forever waiting for the window to open the movie).
8. Input natives 0x406000/0x406040: "no joystick" → the game uses the keyboard pad emulation
   (stub returned 1 = mascon controller, keyboard was ignored).
9. `RegisterModel`/`UnregisterModel` (0x411600/0x4117f0) natives for 3D models.
10. Message loop pacing: one call of 0x41af10 = one PS2 vsync tick. Ticks are paced to exactly
    **60 Hz** with `SDL_DelayPrecise`. Why exactly 60: the game's limiter (0x4075a0) targets 1/60
    and skips rendering when behind (59.94 → bursts). The original relied on a blocking D3D
    present, which doesn't pace on 120 Hz displays. `RECOMP_TICK_HZ` overrides.
11. Swapchain: 1 frame in flight; present without the extra 1 ms delay.
11a. **Frame limiter replaced** (0x4075a0, native `FrameLimiter`): it anchored a 1/60 s schedule to
    the first frame and skipped drawing for up to 1/6 s whenever a frame finished slightly late,
    which caused stutter in menus and animations. With the precise 60 Hz tick pacing every frame
    is drawn; game logic is frame-locked, so speed is unchanged.
11b. Mesh draws reuse the previous parameter block when identical (one draw per triangle strip,
    ~7,000 per frame while driving): GPU uploads dropped from ~120 MB/s to ~18 MB/s.

Port (`port/src/unb`):
12. Mesh per-draw parameters moved from uniform pushes into storage buffers indexed by
    instance id (`vs_mesh`/`fs_mesh`) — cheaper and avoids per-draw uniform traffic.
13. Fog factor clamped per fragment.
14. GS `TEST` AFAIL modes emulated (`ApplyWriteMasks` in Gs.cpp): ATST=NEVER + AFAIL=ZB_ONLY
    now writes depth only (colour write mask), FB_ONLY/RGB_ONLY likewise. New render state
    `RS_COLORWRITEENABLE` (168) in CGraphic. (Original D3D port drew nothing here.)
15. Mesh pipelines use depth clamp instead of far-plane clipping.
16. `ClearBorders` runs every frame (also with screffect) → no text trails in the margins.
17. GS kick with z = 0 maps to depth 1.0 (the original's 0.99999994 was 1.0 in its D24 buffer).

---

## 5a. Sound

The game's own sound code is translated: `CSound`/`CSample` (0x407ea0–0x40a2e0: 25 voices at
0x670b08, volume/pan/pitch, streaming threads) and the `CWave` RIFF reader (0x412d10–0x413600).
`host/dsound.cpp` supplies what sits underneath:

* **DirectSoundCreate8** (`DSOUND #11`): fake IDirectSound8 / IDirectSoundBuffer8 /
  IDirectSoundNotify objects (guest structs `[vtable][index]`, vtables of `rt_host_fn`s).
  Buffer memory is guest heap, because the game writes samples through `Lock`.
* **Mixer thread**: linear-interpolating mixer for 8/16-bit mono/stereo PCM, keeping about
  40 ms queued in an SDL3 audio stream (48 kHz stereo float), with soft limiting above 0.75.
  Volume and pan are in hundredths of dB, as in DirectSound.
* **Streaming**: music (`cddata/dengo/music/*.INT`, 22 kHz stereo WAV) and long sounds use
  a looping buffer with 16 notification positions. The mixer signals those events as it passes
  them, taking the runtime lock (`rt_lock_host`) because it is not an emulated thread. Game
  threads then refill one segment (about 0.19 s for music).
* **mmio**: `mmioOpenA/Read/Seek/Descend/Ascend/CreateChunk/Write/Close` over host files.
  All sound data is plain PCM WAV, whatever the extension (`.wag`, `.vag`, `.INT`).
* `CSound` init (0x409060) is called from the native WinMain after `CGraphic::Init`, as the
  original did. It is a translation root in `build_game.py` for that reason.

Debug: `RECOMP_TRACE_SOUND=1` (files, buffers, play calls; `=2` adds every `Lock`),
`RECOMP_SOUND_DUMP=<file>` (records the mix as raw 48 kHz stereo float32),
`RECOMP_NOSOUND=1` (no audio device; the mixer still keeps time, so streaming logic runs).

## 6. Debugging toolkit

All hooks are environment variables (times are **tick time** = ticks × 1000/60 ms, so runs are
reproducible):

| Variable | Effect |
|---|---|
| `RECOMP_KEYS="ms:vk[:dur],…"` | Press Windows VK code (hex) at tick-time ms for dur ms |
| `RECOMP_SHOT_AFTER=s1,s2,…` | Screenshots → `recomp/snapshot/*.bmp` |
| `RECOMP_MEMDUMP=s1,s2` | Dump 0x4b2000–0x860000 + 32 MB heap to `build/mem<n>.bin` (diff to find variables) |
| `RECOMP_TRACE_TICKS` | Every 10 s: tick rate, game clock (0x717fc4), message-loop phases, presents (late > 20 ms, worst gap), mesh draws, work per tick, GPU flushes/uploads, live textures/meshes, memory. The first thing to run for performance problems |
| `RECOMP_TRACE_TICKSEQ[=2]`, `RECOMP_TRACE_PRESENT` | Per-tick frame production, present interval histogram |
| `RECOMP_TRACE_PAD` | Pad/keyboard state every 250 ms |
| `RECOMP_TRACE_REGS`, `RECOMP_SHOT_TRACE` | GS register values / vertex kicks of the screenshot frame |
| `RECOMP_TRACE_TEX`, `RECOMP_DUMP_TEX` | Texture loads/updates; decoded textures → `recomp/tex/` |
| `RECOMP_GPUTEX="s:handle"` | Compare a managed texture's GPU copy with the CPU copy |
| `RECOMP_TRACE_STUBS` | Which weak native stubs are called and how often |
| `RECOMP_TRACE_STATS` | DMA/GS counters every 120 frames |
| `RECOMP_TRACE_FLUSH=<frame>` | who triggered each GPU submission in that frame |
| `RECOMP_TRACE_MESHREPEAT` | distinct models vs. model draws per frame |
| `RECOMP_PREF_DIR=<dir>` | use another settings/saves folder (benchmarks) |
| `RECOMP_FORCE_SKIP=<n>` | skip n frames after each drawn one (a slow device's frameskip pattern) |
| `RECOMP_SHOT_WINDOW=<s>` | GL: save the final window image once (`snapshot/window.bmp`, includes the `fps` counter) |
| `RECOMP_TRACE_SPIKES=<ms>` | breakdown of every tick / GL job slower than that |
| `RECOMP_GL_THREADED=0\|1`, `RECOMP_GL_SLOW=<ms>` | GL render thread off/on; delay per frame on it (slow-GPU imitation) |
| `RECOMP_GL_DEBUG=<bits>` | GL renderer: 1 no culling, 2 no depth test, 4 opposite cull direction |
| `RECOMP_GL_EXP=<bits>` | GL GPU-cost split: 1 trivial fragment shader, 2 draws scissored to 1 pixel, 4 no present blit |
| `RECOMP_GL_ASYNC_PRESENT=0`, `RECOMP_GL_NO_SHADER_CACHE`, `RECOMP_GL_NO_TEXARRAYS` | GL: present on the render thread; no `shaders.bin`; texture arrays off even if `texarrays = 1` |
| graphics.ini `maxfps = <n>` | draw at most n frames per second (60/n ticks apart), game logic stays at 60 Hz |
| `RECOMP_GL_EARLY_UPLOADS=0`, `RECOMP_GL_UPLOAD_BUDGET=<ms>` | GL: no texture uploads ahead / stand-ins (3b, scenery changes); per-frame upload time before model textures get stand-ins (default 3, 0 = always wait) |
| `RECOMP_PROFILE=<file>` | Linux: CPU sampling profile (1 sample per ms of CPU time, all threads), written at exit; `tools/profile.py <file> <binary> [top] [thread]` prints the hottest functions per thread |
| `RECOMP_EXIT_AFTER=<s>` | quit after s seconds of tick time (scripted runs, PGO) |
| `RECOMP_CLEAR_FLUSH=1` | old render-pass clears (A/B comparison) |
| `RECOMP_TRACE_VIEWPORT` | SetViewport calls (the game never calls it so far) |
| `RECOMP_SKIP_MESH=all\|0-3`, `RECOMP_SKIP_TEX_SEQ`, `RECOMP_SKIP_DIFFUSE_SEQ`, `RECOMP_SKIP_2D[_SEQ]` | Bisect draws (skip by shader / texture / index) |
| `RECOMP_TRACE_DIFFUSE`, `RECOMP_TRACE_MESHTEX`, `RECOMP_TRACE_2D` | Per-draw logs |
| `RECOMP_DEBUG_MESH_{UV,ID,TEX,READ,COLOR,SHADE,PARAMS}` | Mesh fragment shader debug outputs |
| `VERIFY_KEEP=1` (verify.py) | Keep the generated C file and dylib in `build/` |

Scripted navigation (run from `recomp/`): `. tools/drive_keys.sh` sets `$K` to reach the Yamanote
Line, timetable 01 (driving starts ≈ 65 s tick time). `tools/nav.sh` runs it, rebuilds on discovery
traps and converts the screenshots to `build/shots/*.png`:

```sh
. tools/drive_keys.sh
tools/nav.sh "$K" "72" 75          # screenshot at 72 s tick time, 75 s timeout
python3 tools/magcount.py          # count magenta pixels in snapshot/*.bmp
```

Useful addresses: game clock seconds `0x717fc4`; keyboard state `0x6708e0`; pad struct ptr
`0x758270`; input mode `0x670a60`; frame limiter `0x4075a0`; flip/vsync function `0x419270`;
scheduler tick `0x41af10`; timer util `0x401090`; GS globals `0x673080..`.

---

## 6a. Performance work and how to measure

Benchmarks (run from `recomp/`, need the game folder via `DENGO_FINAL_DIR` or a saved `game.ini`):
- `tools/bench.sh`: scripted drive into the Yamanote Line with fixed settings (`tools/bench-prefs/`,
  640×480, draw distance 0.6, via `RECOMP_PREF_DIR`), prints game/render ms per tick at 70/80/90 s.
- `tools/ab.sh <binA> <binB> [rounds]`: alternates two binaries under the same conditions. Copy
  binaries with `rm -f dst && cp` (overwriting a binary in place makes macOS kill it at launch).
- The `RECOMP_TRACE_TICKS` line also has **instructions per tick** for game code and for the render
  path (counted per thread while holding the runtime lock, macOS only) and process CPU per tick.
  Wall time on Apple cores hides most micro-optimisations; instruction counts are the better proxy
  for in-order handheld CPUs (Cortex-A35 budget ≈ 16 M instructions per 60 Hz frame).

Measured at the benchmark point (instructions per tick on the game thread):

| Step | Game logic | Render path |
|---|---|---|
| start of the performance work | ~9 M (est.) | 16.5 M |
| cached `getenv`, strips merged per model, D3DX/CRT natives, O(1) dispatch | 7.3 M | 16.5 M |
| render state set only when changed | | 14.5 M |
| one shared vertex arena for all models | | 10.4 M |
| depth clears as quads, 2D draws merged | | 10.1 M |
| ThinLTO (`LTO=1`, default) | 6.8 M | |
| x87 stack in local variables where provable (`fpu_info` in `x86lift.py`) | 6.5 M | |
| VU0 math wrappers native (`0x415700-0x4158c0`, `host/d3dx.cpp`) | 5.95 M | |
| MSVC helpers inlined as intrinsics (`__allshl`, `__ftol`, 64-bit mul/div) | 5.7 M | |
| small straight-line leaf functions inlined at call sites | 5.5 M | 10.0 M |
| registers passed as C arguments, EAX/EDX/ECX/ESP returned in registers (`RtRet`) | 5.3 M | |
| 64-bit signed address arithmetic, signed `esp`, guest memory with a 4 GB alias | 4.8 M | |

Translator switches (environment, at `build_game.py` time): `LIFT_NO_FPU_LOCALS`,
`LIFT_NO_INTRINSICS`, `LIFT_NO_INLINE`, `LIFT_NO_ADDR64`. `verify.py` runs each function in a
forked child (a crash fails only that function; no state carries over), with Unicorn set up once.

**Calling convention** (`recomp_rt.h`): translated functions are
`RtRet F_x(eax, ecx, edx, ebx, esp, ebp, esi, edi)`; EAX/EDX/ECX/ESP come back in the 16-byte
`RtRet` (registers on ARM64/x86-64), EBX/EBP/ESI/EDI through `R`. Natives and indirect calls keep
the `R`-based `void(void)` form; `dispatch.c` wraps each translated function in an adapter `A_x`.
Macros: `RT_CALL`, `RT_RET`, `RT_RET_FROM_R`.

**Addressing** (`RtAddr`, `host/guestmem.h`): memory accesses compute
`sext32(base + index*scale) + disp` in 64 bits, so the displacement folds into the load/store.
The guest memory is mapped twice, the 4 GB at `MEM` and the same pages again in the 4 GB below,
which makes every result in [−4 GB, 4 GB) identical to x86's 32-bit wrap-around. `esp` is a
signed local, so consecutive pushes share one base register (paired stores). `lea` keeps exact
32-bit arithmetic. `MEMBASE` must leave room for the alias below it, and on macOS the alias must stay clear of
malloc's 0x7000000000–0x8000000000 (default 0x10000000000).

Count translated calls with `make DEFS="-DRT_MEM_BASE=0x10000000000 -DRT_COUNT_SAVES"`
(~46,000 per frame at the benchmark point).

## 7. Known issues / next steps

1. **Thin magenta/coloured 1-px lines** in the sky along rows that are multiples of 32 (fixed
   2026-10-13, SDL GPU renderer): the screen effect blends the scene into itself, a draw that
   samples texture slot 0 while slot 0 is the render target (shifted by 1.5 lines, after the
   draw that blends in the previous frame from slot 1). Reading the target being drawn is
   undefined, and Apple GPUs returned garbage along their 32-pixel tile rows. Such draws now
   start a new render pass after a copy of the target (`pTargetCopy`, `DrawCmd::copyTarget`)
   and sample the copy. The earlier suspects (depth precision, far-plane clipping, implicit
   LOD) were not the cause. `GraphicGL.cpp` had the same feedback loop (no visible artifact
   with Apple's GL driver; undefined on tile-based Mali too): it now copies the bound FBO into
   `targetCopy` with `glCopyTexSubImage2D` before such a draw and samples that.
2. **Canvas and margins** (resolved by `crop`): the game draws its picture at (20,30)–(620,450)
   of the 640×480 layout and relies on TV overscan for the rest. The original stored the GS
   SCISSOR register but never applied it, so 3D spills into the margins while fades and cinema
   bars cover only the canvas. The present pass now shows just the canvas at 4:3. Internal
   screenshots (`PrintScreen`) still contain the full target.
3. **Performance** (GL, R36H, see 3b): ~60 frames/s standing, ~45-60 while driving. Left: a
   scenery change still stops the picture for ~100 ms, now mostly the game's own loading tick
   (~40 ms of translated game logic, ~14 ms creating/freeing textures and models; the copy of
   each texture for the render thread is ~6 ms of it); the 60 frames/s on the 78 Hz panel are
   shown for one or two refreshes each (uneven, see 3b); the 2D HUD is ~100-130 draw calls per frame (texture
   changes ~45, blend state ~35); the game thread is at ~15 of 16.7 ms per tick (game logic
   ~10 ms, recording ~5 ms) when every frame is drawn.
4. **Sound**: works (see 5a). Not done: 3D/position effects (the game doesn't use them on
   PC), tuning of the overall loudness against the original.
5. **Movies**: skipped (DirectShow). Could be decoded with FFmpeg if wanted.
6. **Joystick/mascon controllers**: not supported (0x405ef0/0x406000/0x406040 report none).
7. SDL GPU has Metal (MSL source) and Vulkan (SPIR-V, see 3c) shaders; D3D12 (DXIL) is missing.
   Linux handhelds without Vulkan use the GLES backend (3a).
8. 118 functions fail translation (MMX/SSE/3DNow! D3DX math). Not hit so far; if a trap reports
   one, add the instructions to `x86lift.py` or make the function native.
9. The debug hooks in §6 are compiled in permanently; they cost nothing unless set.

How to tackle a new crash: run `./iterate.sh` (handles discovery); for other traps read the
backtrace (`stack:` list of return addresses), look the function up in Ghidra (or a local `out/perfect_all.c`), and
decide between fixing translation, implementing a missing import in `win32.c`, or a native in
`native.cpp` (+ `plan.py`).
