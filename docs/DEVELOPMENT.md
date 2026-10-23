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
| `recomp/builder/` | The builder kit (§3k): `builder.py` (runs on the user's machine), `make-kit.py` (makes the kit), `README.txt` (the kit's) |
| `recomp/plan.py` | Which address ranges/functions are native instead of translated |
| `recomp/build_game.py` | Discovers functions, translates, writes `build/game/*.c` |
| `recomp/verify.py`, `gen.py`, `rt_test.c` | Differential tester (Unicorn vs translated C) |
| `recomp/host/runtime.c` | Memory, PE loader, dispatch, threads/GIL, heap, path mapping |
| `recomp/host/win32.c` | Win32 API shim |
| `recomp/host/os_win.c`, `host/win/include/` | Windows builds: the POSIX subset the runtime uses, emulated memory, fibers, crash report (§3m); `recomp/windows/build-cross.sh` |
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
  driver reserves ~12 GB at a random address once a window exists, and sometimes covered the
  fixed base (0x8000000000); that was the intermittent "cannot reserve the emulated memory".
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

### 3f. Game tasks as fibers (2026-10-13)

The game's ~13 cooperative tasks (docs/TASKS.md) each had a guest thread; every tick about 14
hand-overs went through SetEvent + WaitForSingleObject, i.e. a futex wake-up and sleep across two
host threads. Profile on the PC (Osaka Loop, 60 frames/s): 27% of the guest threads' CPU time in
system calls, ~57 us per hand-over. The scheduler (0x41ab00-0x41af70) is now native
(`host/tasks.cpp`, `plan.NATIVE` "Tasks") and every task is a fiber (`rt_fiber_create`,
`rt_fiber_switch` in `host/runtime.c`: guest CPU state and TIB swapped like a lock hand-over, host
registers and stack swapped by `rt_ctx_switch`, assembly for x86-64 and arm64, ~16 ns on the PC;
checked on arm64 under qemu). Each function follows the disassembly; the C runtime's per-thread
data is allocated, entered and freed as `_beginthread`/`_threadstart`/`_endthread` did. Guest
stacks of ended fibers are cleared and reused (new threads take a fresh 1 MB each, so ~250 mode
changes would have exhausted the stack region). `0x41c490`/`0x41c4b0` (allocator used by the
scheduler) are translation roots now.

PC, Osaka Loop drive: game thread 2.25 -> 1.5 ms per tick (first part), 1.6 -> 0.97 ms (second
part); process CPU -10..15%; idle tasks 0.001 instead of 0.02 ms per tick. Compared with the
threaded build with maxfps 0 / frameskip 0 (deterministic frames), the drive is pixel-identical
(100 and 140 s); only the menu's background slideshow can be at another picture (load timing).
(An earlier comparison with maxfps 30 and frame skipping showed differences in rain and in the
distance after the ATS stop: frames one tick apart, not a change.) A 6-minute run with game over,
Continue and a restart: no errors, heap flat. R36H: game thread -1.2 ms per tick (-16%).

### 3g. Draw distance: the game's own, or fixed (2026-10-13)

The original sets the draw distance itself: every tick the 3D scene (`0x47d940`) calls
`0x47e4f0`, which stores the result of `0x47e1d0` (it reads the train's speed, but gave 0.45 at
every speed measured on the Osaka Loop: 0, 24 and 70 km/h) into RendPow `0x5db0f8`, resets OkiDistArg to 0.4 and
scales further distances by it. DDGModernizer's [DrawDistance] patches remove that ("Fix
magnification", "Fix for outer view"), so its fixed value applies.

Since `exepatch.py` applied all of DDGModernizer's patches (2026-10-12), `drawdistance = 0.90`
really meant 0.90, twice the game's own distance: on the R36H 2201 instead of 1091 mesh draws per
frame, and the game fell to 43-53 ticks/s. Builds before that (from exes without DDGModernizer's
patches, like the R36H's) ignored the setting. Now the canonical exe keeps the original
[DrawDistance] code, and a fixed draw distance is a run-time switch (`plan.SETTING_DD_FIXED`):
`MEM_OVERRIDES` sends the computed value to a scratch slot (0x47e504), `IMM_OVERRIDES` pushes the
player's okidist instead of 0.4 (0x47e50a), `BRANCH_OVERRIDES` takes the cab-view rule in every
view (0x423621). `drawdistance = auto` (default, also in the settings window) keeps the original
behaviour. PC, Osaka Loop: auto 480-610 mesh draws per frame, 0.90 1030-1230.

### 3h. Where the R36H's GPU time goes (2026-10-13)

Osaka Loop drive in Driver Mode, draw distance auto, 60 frames/s, GPU load from devfreq (performance
governor), leaving out one class of draws with `RECOMP_GL_EXP` (the parts overlap, so they do not
add up): reference 74%; without 2D draws 41% (-33), of which the ~13 full-screen clear quads per
frame (10 depth-only, 2-3 black; 12 screens of area) -6 and the HUD -28; without alpha-tested
meshes 52% (-22: only ~25 draws, but they are the sky dome and the overhead wires, and discard
keeps the Mali from killing sky pixels that are covered later); without blended meshes -9;
without opaque meshes -10 (~1100 draws); final pass -8 before the gamma table, ~-5 with it;
trivial fragment shader -23; everything scissored to one pixel 18% left (vertex and tiling work).

2D per frame (PC census): 87 textured blended HUD draws covering ~3 screens, 3-4 untextured
blended ones ~0.9 screens, the clear quads 12 screens; no palette-path draws. A textured full-
screen pass costs the G31 ~5-7 points at 60 frames/s, so the HUD's cost is its area. Issuing the
clear quads as glClear was tried and is slower (+2.5 points; mid-frame clears are not cheaper on
this driver). Drawing alpha-tested meshes after the opaque ones in sorted runs: no gain (and no
change of the picture; the old order already matches the unsorted game order).

**HUD quads cropped** (`CropQuad`, `RECOMP_GL_CROPQUADS=0` to disable): 61% of the HUD's texels
are fully transparent and 32% of its area lies outside the box of the visible texels. Textured
2D quads whose transparent texels cannot change a pixel (source weighted by alpha, alpha from the
texture: SELECTARG1 / MODULATE / MODULATE2X; axis-aligned, u along x and v along y, no repeat,
constant colour and depth) are cropped to that box plus one texel (bilinear), texture coordinates
mapped as before; boxes cached per texture region and texture version (`contentGen`). Frames
identical (PC, maxfps 0: 0-31 pixels > 8; R36H 24). R36H GPU load 80.4% -> 74.4%.

**Alpha-tested meshes (sky, wires, gantries):** ~22 alpha-tested draws per frame, all with binary
alpha (0 or 255) and ALPHAREF 0.624, depth write on, mostly unblended; 33-81% transparent texels;
textures 32x32 to 512x512, no mipmaps (only fully opaque textures get them). Tried on the R36H:
plain box mipmaps for them -5 points, but distant overhead-line gantries vanish (the gain is the
pixels they no longer cover); coverage-preserving mipmaps (alpha scaled per level so the share of
texels passing the test stays that of level 0) keep them (dotted at a distance) and gain nothing
(74.5% vs 76.0%). So their cost is the covered pixels and the discard, not texture fetches;
neither was kept.

### 3i. Longer draw distance on the R36H: CPU work per frame (2026-10-13)

Goal: the overhead-line gantries far ahead are missing at the game's own draw distance (`auto`,
0.45). R36H, Osaka Loop drive in Driver Mode, frames per second at 80-100 s (previous build):

| drawdistance | far gantries | frames/s |
|---|---|---|
| auto (0.45) | partly missing | 60 |
| 0.60 | the bridge and most gantries | 58-60 |
| 0.75 | nearly all | 50-58 |
| 1.00 | all | 38-48 |

At 1.00 per frame: game thread ~9 ms game logic + ~10 ms walking the DMA chain (VIF, VU1 packets,
GS, recording); render thread ~19 ms (draw list 8, uploads 2, submit 9). Both over 16.7 ms.

- **Present clamp:** with `crop`, bilinear filtering at the outermost window pixels mixed in a
  little of the overscan margin (3-6% of their colour); the final pass now clamps its texture
  coordinates half a texel inside the shown canvas (GL `uClamp`, SDL GPU `PresentParams`). The
  black top row in menus is the game's own picture (its menu art starts one row below the canvas).
- **Profiler:** samples carry up to 4 return addresses from the frame-pointer chain (arm64),
  `PROFILE_LINES=1` counts per source line. ITIMER_PROF is process-directed: its signals also
  land on threads that are asleep (nanosleep between ticks, futex waits), which showed up as ~10%
  "libc" on the game thread; those samples are someone else's CPU time.
- **Submit:** ~20 us per draw call in the Mali driver regardless of size (one triangle per batch:
  9.1 -> 8.5 ms; no mesh draw calls: 3.8 ms). Most batches are split by texture changes. Texture
  arrays (`texarrays = 1`) re-measured: draw calls 346 -> 195, submit 8.7 -> 4.9 ms, but the GPU
  becomes the limit (40 frames/s), as before.
- **List thread** (`ListMain`, `RECOMP_GL_LISTTHREAD=0` to disable; not with texture arrays):
  the draw list of a job (`BuildList`, buffers per job in `Job::List`) is built on its own thread
  while the render thread submits the previous job; the render thread takes a job once its list is
  built. Render thread at 1.00: ~19 -> ~12 ms per frame.
- **DMA thread** (native.cpp, `RECOMP_ASYNC_DMA=0` to disable; GL with its render thread): the
  frame driver sends the frame built during the previous tick and the tasks then build the next
  one into the other half of the double-buffered packet memory, so the chain is walked on its own
  thread while the game computes the next tick. Walked in place, frames were black: the game
  reuses the chain's own memory right away. The send now copies the chain (`CDma::Flatten`): data
  inside the chain (cnt/next/call tags) is copied, and so is data that ref tags point to, unless it
  lies inside a registered model's packet block (`CVertex::InModelPackets`; those change only with
  model (un)registration): 1.4 ms per tick at 1.00 instead of 3.1 for copying everything
  (`RECOMP_DMA_COPYREFS=1`). Everything from game code that reaches the packet path's state waits for the chain first
  (`DmaSync`): the next send, `sceGsSwapDBuffDc`, texture load/unload/update, model
  (un)registration, viewport and gamma. Texture `Load` and `RegisterObject` are called ~90 and
  ~120 times per tick for scenery that is already loaded; those calls do not wait (else the game
  thread waited 6-8 ms per tick).
- **Recording:** the vectors handed back by the job pool are reserved to the last frame's size
  (they grew while recording every frame), parameter sets are built in place and compared inline
  (matrix first) instead of with `memcmp`.
- **Batched vertices** (`PreVertex`): normals and UVs as the game's int16 fixed point, scaled back
  in the batch shader: 24 instead of 32 bytes to copy and upload, exact.
- Measured and not done: keeping each slot window's geometry on the GPU when the next frame has
  the same draws in it. While driving only 25-39% of the windows repeat (99% when standing): a
  model entering or leaving shifts everything after it in the sorted order. 16-bit indices: no
  base-vertex draws in GLES 3.0, and absolute indices would only fit below 65536 vertices.

Frames identical throughout (maxfps 0, frameskip 0; 0 pixels > 8 at 70, 90, 100 s against the
previous build, and between repeated runs at 50, 70, 100 s). The route's intro flyover (50 s)
differs between modes by ~60 pixels but repeats within a mode; it depends on timing (loading
against ticks): the synchronous build with only the list thread switched off differs by 1575. The R36H throttles to 1008 MHz within
about a minute of this load even when started cool (55 C), so the comparisons below are at that
clock (each run started below 55 C):

| | 0.75 | 1.00 |
|---|---|---|
| before | 50-58 frames/s, GPU 74% | 43-49, GPU 68% |
| list thread + DMA thread | 53-56, GPU 81% | 51-53, GPU 83% |
| + recording, packed vertices, model packets in place | | 52-54, GPU 82-84% |

At 1.00 the critical path is now the DMA thread (~10.5 ms per chain at 1008 MHz plus waiting for
the render thread) and the GPU (83%); all four cores are busy. The threads cost more CPU in total
(more heat). For 60 frames/s on the R36H: draw distance 0.60.

### 3j. Automatic draw distance, PSP comparison, options menu (2026-10-16)

**PSP version** (Densha de GO! Pocket Chuo-sen-hen, 2005, same engine), compared with FINAL's
`cddata/mapdata/chuoA_D`: the same container format (`DAT`: count, then offset/size pairs) and
76 area packs; `map.dat`'s track/section parts are byte-identical. Objects carry three model slots
(+0x10 on PC, +0x08 on PSP), which are parts of the object (e.g. a signal's lamp head), not
levels of detail: neither version has distance LODs. The PSP removed 8% of the objects (trees,
houses, parked cars, small props, ground pieces), has 27% fewer triangles (1.97 M vs 2.69 M over
the route; mostly simplified station buildings) and nearly as many draws; its vertices are 12
bytes (GE vertex type 0x121/0x122: 8-bit UV and normal, 16-bit position, triangle lists).

**The draw distance controller.** Both have the same function (PC `0x47e1d0`, PSP `0x3ef84` in
BOOT.BIN, an unencrypted MIPS ELF): every tick it compares the frame's load with a budget and
moves the draw distance by 0.02 (far over or under), 0.008 (under) or |speed| x 0.0016, averaged
over 60 ticks (PSP 30), between 0.45 and 1.0 (PSP 0.625-1.0). The result scales the base
distances 300/135/300/500 (PSP 270/135/270/400; the debug line "pow : %3dm" prints x500).
- PC: load = max(cor, dra), ms x 31.5 / x 3.15 (`0x417510`, `0x417580`); budget from the registry
  value "rendpow" (`0x4c8c4c` table {0,115..175}, default 155 = 4.9 ms of "cor"). "cor" spans from
  the end of one frame-driver run to the start of the next, i.e. the whole tick including the
  frame limiter's wait (~525 at 60 frames/s), so the PC game only ever shrinks: always 0.45.
- PSP: load = CPU time x 7650/s, budget 215 = 28 ms: it targets 30 frames/s.
- PC only: after the average, a hold (`0x730c60`) is set the first time the average falls, and the
  result is capped at the hold, which only ever falls (reset at a route's start, `0x455254`,
  which also starts the average at 0.65). One loading spike lowered the distance for good.

**Auto now** (`drawdistance = auto`; `original` keeps the old behaviour): `MEM_OVERRIDES` make the
controller read the host's load, budget, limits and steps from the settings page, and
`BRANCH_OVERRIDES` 0x47e3d2 never sets the hold (as on the PSP). The load (`DrawLoadTick`) is the
busiest stage's share of a drawn frame's period, in the controller's units (525 = one period),
smoothed over ~16 ticks: game thread work over the frame's ticks (it includes waiting for the DMA
and render threads), the DMA thread's last chain, and the renderer's slowest stage
(`CGraphic::FrameLoadNs`: list thread, or render thread plus its wait for the GPU to finish the
previous frame, about max(render thread, GPU)). Budget `ddload` 0.80; limits `ddmin` 0.45,
`ddmax` 1.0; steps 0.006 down, 0.002 / 0.0008 up per tick (the original's 0.02 crossed the range
within a second: with the noisier load the distance swung and scenery popped in and out).
`RECOMP_TRACE_DD=1` prints load, its parts, the distance and the speed every tick. With a fixed
distance the controller keeps running into the scratch slot, so switching to auto continues from
an adapted value.

R36H, Osaka Loop drive (Driver Mode), 80-100 s:

| | draw distance | ticks/s | frames shown/s |
|---|---|---|---|
| maxfps 30, frameskip 3, auto | 1.00 throughout (load ~45%; CPU at 1512 MHz 98% of the time) | 60 | 29-30 |
| maxfps 0, frameskip 0, auto | 0.45-0.9, average ~0.62 (stations: game and DMA threads ~20 ms) | 59.1-59.6 | 59 |
| maxfps 0, fixed 0.60 (3i) | 0.60 | 58-60 | |

**Options menu** (`host/overlay.cpp`; gamepad Y, F10): resolution (after a restart), draw
distance (auto, or fixed 0.30-1.50), max distance (auto's `ddmax`), frame rate (60/30/20/15) and
the fps counter (GL). Changes apply at once and are written to graphics.ini when it closes
(`UpdateIniValues`, only the keys that changed). It is rasterised with its own 5x7 font into a
264x122 RGBA image that both renderers draw centred at an integer scale over the picture
(`CGraphic::SetOverlay`). While open the game keeps running without input; after closing, input
stays blocked until every key is released. `RECOMP_KEYS` can open it with vk 79 (F10).

### 3k. The builder kit: packages from the user's exe without build tools (2026-10-22)

The packages contain the translated game code and cannot be shared. The builder kit is everything
else, already compiled, so that the step involving the game runs on the player's machine with
nothing to install: `recomp/builder/make-kit.py <host>...` writes
`build/kit/ddg-recomp-builder-<host>.(tar.xz|zip)` with

* `python/`: portable CPython 3.12 (python-build-standalone), with capstone unpacked from its wheel
  into `recomp/pylib`. The Linux libpython is stripped of debug info (203 to 38 MB). Stripping the
  `python3.12` executable breaks its symbol lookup, so it is left alone.
* `zig/`: zig 0.14.1 (clang 19, lld), which cross-compiles to `aarch64-linux-gnu.2.28` and
  `x86_64-linux-gnu.2.35` from any host with its own glibc stubs and libc++. No sysroot or Docker is
  needed on the user's side. Only the headers of those two targets are kept (the rest is ~100 MB).
* `recomp/`: the translator (`build_game.py`, `x86lift.py`, `plan.py`, `pe.py`, `exepatch.py`,
  `data/`, `build/discovered.txt`, `recomp_rt.h`) and `builder/builder.py`.
* `targets/<handheld|desktop>/`: the host and port objects (`make hostobjs` with zig as CC/CXX; the
  handheld ones with `-mcpu=cortex_a35` and `linux/perfect.profdata`), `libSDL3.so.0` (from the
  ddg-recomp-arm64 / ddg-recomp-desktop images), the package's files and `target.json`. The JSON
  holds the game code's flags (the Makefile's rule for `build/game/*.c`: keep them in step), the
  link line and the package layout.

`builder.py` runs `build_game.py` with `RECOMP_GAME_DIR` (a work folder), compiles the 21 files in
parallel (thin LTO, so the compile is quick and the codegen happens at the link), links with the
kit's objects and writes the zip / tar.gz with the same layout as `linux/package.sh` and
`linux/build-desktop.sh`. It takes 75-85 s on an 8-core PC, with about 30 s more on the first run,
when zig builds glibc stubs and libc++ for the target into `cache/`. zig reads the clang 16 profile
without mismatch warnings.

Checked: the Linux kit, freshly unpacked, builds both packages. The desktop binary drives the
Osaka loop at 60 ticks/s, and its frame matches the Docker build's (including the sky noise in
§7 item 10, which both show). Wrong files (not a PE, another program, a truncated exe) are refused with the translator's
message (`exepatch.normalize` now also rejects patch sites cut off by the end of the file). Not
checked yet: the zig-built handheld binary on the R36H (clang 19 with libc++ instead of clang 16
with libstdc++: speed, startup), and the Windows kit on Windows. Under wine the translation runs,
but zig stops with "unable to find zig self exe path", a wine limitation.

### 3l. The shareable program: translator and compiler built in (RECOMP_JIT, 2026-10-23)

`make RECOMP_JIT=1` builds the program without the game code, so it may be published (the
release pipeline, `.github/workflows/release.yml`). On the first start with an exe it does what
the builder does, inside the program:

1. **Translate** (`host/pyembed.cpp`): a static libpython 3.12 (python-build-standalone "full";
   `host/mkpyruntime.py` writes a module table with only the 46 built-in modules the translator
   and Python's start-up need, so OpenSSL, SQLite and Tk stay out, and `link.txt` from
   `PYTHON.json`) runs `build_game.py`. The standard library modules it imports (2.1 MB zipped),
   the translator and capstone are in `runtime.bin`, embedded with `.incbin` and unpacked into the
   cache once. Paths go to Python as variables of `__main__`: `setenv()` on the translating thread
   raced with `getenv()` in SDL and the settings window.
2. **Compile** (`host/jit.cpp`): clang as a library, the cc1 options clang 23 uses for the
   Makefile's `build/game/*.c` rule (without the stack protector), one thread per core, the C
   library declarations built in (no system headers needed).
3. **Link**: LLVM's JIT (LLJIT/JITLink) adds the objects to the running program. Host symbols
   come from the program (`--export-dynamic-symbol` for `N_*`, `R`, `RT_*`, `rt_*`, so that
   `--gc-sections` can drop the unused parts of LLVM). `native_info.c` defines every native
   weakly (a trap) for the static linker to replace: the JIT keeps the first definition, so the
   program's natives are defined first. The tables runtime.c needs (`RT_TABLE`, `rt_exe_*`) are
   looked up afterwards (`rt_jit_sym`).

Caches (`~/.cache/ddg-recomp`, macOS `~/Library/Caches/ddg-recomp`): `runtime-<hash>` (the
unpacked translator), `game-<hash of exe and runtime>/src` (the C; `.ready` once compiled),
`jit-<hash of the C, the header and LLVM's version>` (the objects). Before the game code exists
`rt_check_exe` only checks for a PE file without SafeDisc; the translator checks the version and
the full check runs after loading.

The settings window prepares the game code in the background (`LauncherPrepare`: translating,
"Compiling 7 of 21 files", Ready; Start waits for it) and opens on its own while an exe has no
game code yet. It now also has the exe path and the options menu's settings (max distance, frame
rate, fps counter). Test hooks: `RECOMP_LAUNCHER_SHOT=<s>,...` (saves the window as
launcher-<n>.bmp), `RECOMP_LAUNCHER_AUTOSTART=1`.

Measured on the PC (Ryzen 7 4750U, 16 threads), static LLVM 23.1.1 (x86 only):

| | |
|---|---|
| first start, 16 threads | 48 s (translation ~27 s, compile 19.7 s) |
| first start, 4 cores (`taskset -c 0-3`) | 65 s (compile 34.5 s) |
| later starts | +0.1 s (objects from the cache, link 40 ms) |
| game code per tick, Osaka loop drive | 1.5-1.9 ms, as the normal clang build; frame identical |
| program, stripped | 106 MB (LLVM/clang ~75 MB, runtime.bin 12.7 MB); 27 MB as .tar.xz |

python-build-standalone's static libpython exists only as clang 20 LTO bitcode: the link needs
clang/lld 20 or newer (the release scripts use LLVM 23 from apt.llvm.org and Homebrew). The
release scripts (`recomp/linux/release-deps.sh`, `build-release.sh`, `recomp/macos/build-release.sh`)
pack the program with SDL3, README-release.txt, SOURCE.txt (the commit, for the GPL) and the
licences of everything inside. The macOS script has not run yet (no Mac here): its first runs on
the GitHub runner will show what is missing.

### 3m. Windows (x86-64, 2026-10-23)

`recomp/windows/build-cross.sh` builds `build/win-x64/perfect_recomp.exe` (with the game code,
like build-desktop.sh) on Linux with llvm-mingw (clang 23, UCRT) and SDL3's MinGW package; the
exe needs only SDL3.dll and system DLLs. The renderer is SDL GPU on Vulkan (Graphic.cpp offers
MSL and SPIR-V, so SDL does not pick D3D12, which would need DXIL shaders). The Makefile
recognises a Windows compiler by `$(CC) -dumpmachine`.

The runtime keeps its POSIX code: `host/win/include` (first on the include path of Windows
builds) has the pthread.h, unistd.h, dirent.h and sched.h subset it uses, implemented in
`host/os_win.c` on Win32 (SRW locks and condition variables, `_beginthreadex`, high-resolution
waitable timers for `usleep`, QueryPerformanceCounter, FindFirstFile). MinGW's own POSIX layer
(winpthreads) is not used, so a later MSVC-ABI build (clang-cl, needed for the shareable program:
the Windows builds of python-build-standalone and LLVM are MSVC) compiles the same code.
os_win.c is the only file with `<windows.h>` (its names collide with the Win32 shim's).

- **Emulated memory**: one pagefile-backed section of 4 GB, mapped at `RT_MEM_BASE` and again
  below it (guestmem.h). Only reserved: a vectored exception handler commits 64 KB on the first
  touch, and `rt_guest_commit` before `fread` into guest memory (the kernel's ReadFile fails on
  uncommitted pages instead of faulting). Committing everything would charge 4 GB against the
  commit limit.
- **Fibers** (game tasks, 3f): Windows fibers (`CreateFiberEx`, `FIBER_FLAG_FLOAT_SWITCH`)
  instead of rt_ctx_switch, so that the TIB's stack bounds match the running stack (stack
  probes, and `longjmp`, which unwinds on Windows).
- **Crash report**: an unhandled-exception filter prints the code, module+offset of the pc and of
  a host backtrace (llvm-symbolizer), then the guest registers. No profiler (RECOMP_PROFILE).
- Case-insensitive file names need no lookup (NTFS); `timeBeginPeriod(1)` at start.

Under wine 11 (Vulkan via the host's RADV) the Osaka loop drive runs at 60 ticks/s, game code
1.5 ms/tick as on Linux (the Windows x64 calling convention returns the translated functions'
16-byte RtRet in memory, which costs nothing measurable), render 4 ms/tick against 3.6 natively.
The settings window, sound (WASAPI) and saves work. Not yet tried on a real Windows PC. Still
open: the shareable RECOMP_JIT program for Windows (MSVC ABI, COFF in the JIT) and a release job;
the console window (stderr) and UTF-8 paths (an application manifest with `activeCodePage`).

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

- **Heap tag bug (fixed 2026-10-12):** `rt_heap_alloc` tagged blocks `HEAP_MAGIC | class << 24`
  with a magic whose top byte was 0x48, so the class read back as 72..95. Freed blocks went to
  free-list slots past the 32-entry array (overwriting the statics after it) and were never
  reused, and `rt_heap_realloc` could grow a block in place over its neighbours. Since the fix
  the heap stops growing once a route is loaded (~50 MB live on the Osaka Loop). Freed memory
  is now reused without being cleared, as with the Windows heap. `RECOMP_TRACE_TICKS` reports
  live heap, the VirtualAlloc top (unused during play) and the guest memory's resident size.

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
| `RECOMP_GL_EXP=<bits>` | GL GPU-cost split: 1 trivial fragment shader, 2 draws scissored to 1 pixel, 4 no present blit; leave out 8 2D draws, 16 blended, 32 alpha-tested, 64 opaque mesh draws, 128 full-screen 2D quads, 256 the other 2D draws (see 3h) |
| `RECOMP_GL_ASYNC_PRESENT=0`, `RECOMP_GL_NO_SHADER_CACHE`, `RECOMP_GL_NO_TEXARRAYS` | GL: present on the render thread; no `shaders.bin`; texture arrays off even if `texarrays = 1` |
| graphics.ini `maxfps = <n>` | draw at most n frames per second (60/n ticks apart), game logic stays at 60 Hz |
| `RECOMP_GL_EARLY_UPLOADS=0`, `RECOMP_GL_UPLOAD_BUDGET=<ms>` | GL: no texture uploads ahead / stand-ins (3b, scenery changes); per-frame upload time before model textures get stand-ins (default 3, 0 = always wait) |
| `RECOMP_PROFILE=<file>` | Linux: CPU sampling profile (1 sample per ms of CPU time, all threads), written at exit; `tools/profile.py <file> <binary> [top] [thread]` prints the hottest functions per thread |
| `RECOMP_TRACE_TASKS=1` | with `RECOMP_TRACE_TICKS`: the game's cooperative tasks (id, priority, entry, CPU ms/tick) every 10 s; see [TASKS.md](TASKS.md) |
| `RECOMP_SUSPEND_TASK=<entry>@<s>[,...]` | keep the task with that entry suspended from tick time s (find out what a task does) |
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
32-bit arithmetic. `MEMBASE` must leave room for the alias below it (macOS: ≥ 0x7000000000 + 4 GB).

Count translated calls with `make DEFS="-DRT_MEM_BASE=0x8000000000 -DRT_COUNT_SAVES"`
(~46,000 per frame at the benchmark point).

## 7. Known issues / next steps

1. **Thin magenta/coloured 1-px lines** in the sky along rows that are multiples of 32
   (Apple GPU tile rows). Reduced but not gone. Facts established: only diffuse meshes with the
   sky texture are involved; raw texture sample is correct; the GPU texture copy is correct;
   disappears when 2D draws 4/5 of the frame (full-screen black/grey clears with depth test,
   z = far) are skipped. Suspect: depth interplay between those clears and the far sky
   (precision at tile edges). Ideas: draw the sky with depth test off, or bias mesh depth,
   or compare with a Vulkan run (MoltenVK) to rule out a Metal driver issue.
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
6. **Joystick/mascon controllers**: not supported (0x405ef0/0x406000/0x406040 report none). The
   game's notch variables and input logic are mapped in [TASKS.md](TASKS.md#controls-power-and-brake-notch)
   (power `0x751a04`, brake `0x751a00`, the two input schemes): the starting point for support.
7. SDL GPU has Metal (MSL source) and Vulkan (SPIR-V, see 3c) shaders; D3D12 (DXIL) is missing.
   Linux handhelds without Vulkan use the GLES backend (3a).
8. 118 functions fail translation (MMX/SSE/3DNow! D3DX math). Not hit so far; if a trap reports
   one, add the instructions to `x86lift.py` or make the function native.
9. The debug hooks in §6 are compiled in permanently; they cost nothing unless set.
10. **Noise in the sky with the SDL GPU renderer on Vulkan** (fixed 2026-10-23): the game blends
   its own picture onto itself (a full-canvas quad that samples scene target 0 while drawing into
   it, shifted by one line at half strength, to soften the picture; another quad blends in target
   1, the previous frame). Reading a color target in its own render pass is undefined in Vulkan
   (streaky noise where the sky is). Graphic.cpp now flushes, copies the target and lets such a
   draw read the copy (`FeedbackCopy`), which is what the PS2 saw. The GL builds draw from the
   target itself (a feedback loop too, but the drivers tested give the expected picture).

How to tackle a new crash: run `./iterate.sh` (handles discovery); for other traps read the
backtrace (`stack:` list of return addresses), look the function up in Ghidra (or a local `out/perfect_all.c`), and
decide between fixing translation, implementing a missing import in `win32.c`, or a native in
`native.cpp` (+ `plan.py`).
