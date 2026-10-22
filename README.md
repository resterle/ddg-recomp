# ddg-recomp

**ddg-recomp** is a native port of the PC version of *Densha de GO! FINAL* (TAITO, 2004), made
by **static recompilation**: every x86 function of the game's executable is translated into C,
the Windows and Direct3D 8 parts are replaced with new code on SDL3, and the result is compiled
for the target machine. The game runs without Windows, Wine or an emulator on:

- **Linux desktop** (x86-64): SDL GPU on Vulkan, or OpenGL 3.3
- **Linux ARM handhelds** with ArkOS-style firmware (R36H, R36S and other RK3326 devices):
  OpenGL ES 3.0 on the Mali-G31, packaged as a PortMaster-style port
- **macOS** (Apple Silicon): SDL GPU on Metal

**This repository contains no game code or data.** You need your own copy of the PC game. The
translated game code is generated on your machine from your own executable (see
[Legal](#legal)).

> **AI-generated:** most of the code and documentation in this project was written by an AI
> (Anthropic's Claude, through Claude Code), directed and tested on real hardware by the author.
> Expect the strengths and weaknesses that come with that; bug reports and reviews are welcome.

## Status

Playable from boot to the end of a run: title, menus, all modes tried so far (including the
driving tutorial, Conductor Mode and the Osaka Loop), 3D driving with HUD, sound and music,
saves, keyboard and gamepad controls, correct game speed.

- Not implemented: movies (skipped), the original's joystick/mascon controller support, 3D sound
  positioning (unused on PC).
- 118 functions (MMX/SSE/3DNow! paths of the D3DX maths library) are not translated. None has
  been reached in play so far; reaching one stops the game with a message naming its address.
- On the R36H (RK3326, 4× Cortex-A35 at 1.5 GHz, Mali-G31), driving with `maxfps = 30` and
  `drawdistance = auto` shows the full draw distance (1.0, every overhead-line gantry) at a steady
  30 frames/s; at 60 frames/s (`maxfps = 0`) auto keeps about 59 with a distance of 0.45-0.9.
- An options menu over the game (Y on a gamepad, F10) changes the draw distance, frame rate and
  fps counter while playing, and the resolution for the next start.

## What you need

- The PC version of Densha de GO! FINAL, installed: the `cddata` folder (about 2.6 GB) and an
  **unprotected `perfect.exe`**. The `perfect.exe` on the CD is protected by SafeDisc (its code
  is encrypted) and cannot be used (SafeDisc does not run on Windows 10 and later either, as its
  driver is disabled there). A copy patched by
  [DDGModernizer](https://github.com/hunterbridges/DDGModernizer) works as well: its patches
  are applied by this project anyway, and its settings are options in `graphics.ini`.
- To build: the **builder** (nothing else to install), or **Docker** alone, or Python 3 with
  [capstone](https://www.capstone-engine.org/) 5.0, clang, make and SDL 3.4 or newer.

The recompiled code is generated from your `perfect.exe`, and the game checks at startup that it
runs with the same exe (it reads the game's tables and texts from it). Other versions, or an exe
with a translation patch, need their own build.

## Ready-made programs (Linux, macOS)

The [releases](https://github.com/resterle/ddg-recomp/releases) have programs that contain no part
of the game: on the first start they translate your `perfect.exe` and compile it for your
computer with a built-in translator and compiler (one to two minutes, once; a settings window
shows the progress, and you can choose the game folder, the exe and the settings meanwhile).
They are built by `.github/workflows/release.yml` (`make RECOMP_JIT=1`, see
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) §3l). Linux x86-64 needs glibc 2.35 or newer; the
macOS build (Apple Silicon) is new and not yet tested on a Mac. The ARM handhelds still need one
of the builds below.

## Building with the builder

The builder is a download that contains everything needed to build except the game: a portable
Python with the translator, the [zig](https://ziglang.org/) toolchain (clang and lld for every
target) and the project's own code, already compiled. It translates your `perfect.exe`, compiles
the result for the target and packs the same package as the builds below, in one to two minutes.

```sh
./ddg-builder handheld /path/to/perfect.exe      # -> ddg-recomp-arm64.zip (R36H/R36S, ArkOS-style)
./ddg-builder desktop  /path/to/perfect.exe      # -> ddg-recomp-linux-x64.tar.gz (x86-64 Linux)
```

On Windows, drag `perfect.exe` onto `ddg-builder.bat` (handheld package). The builder runs on
x86-64 Linux and Windows; macOS and arm64 Linux builders can be made the same way but are
untested.

The builder itself is made with `recomp/builder/make-kit.py [linux-x64] [windows-x64] ...` on an
x86-64 Linux machine with Docker (for the SDL3 builds); it writes
`recomp/build/kit/ddg-recomp-builder-<host>.(tar.xz|zip)`. It contains no game code and may be
shared; the packages it builds may not.

## Building with Docker

```sh
./docker-build.sh desktop  /path/to/perfect.exe     # x86-64 Linux (Vulkan; RENDERER=gl for OpenGL)
./docker-build.sh handheld /path/to/perfect.exe     # R36H/R36S and other ArkOS-style handhelds
```

This generates the game code and builds in containers; the results are the same as below
(`recomp/build/ddg-recomp-linux-x64.tar.gz`, `recomp/build/ddg-recomp-arm64.zip`). The desktop binary
needs glibc 2.35 or newer (Ubuntu 22.04, Debian 12 and later). The first run builds the images,
which takes a while; the handheld build also needs qemu for arm64 once:
`docker run --privileged --rm tonistiigi/binfmt --install arm64`.

## Building without Docker

All commands run in `recomp/`.

**1. Python environment** (once):

```sh
cd recomp
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

**2. Generate the game code** from your executable into `build/game/` (well under a minute):

```sh
PERFECT_EXE=/path/to/perfect.exe .venv/bin/python build_game.py
```

Without `PERFECT_EXE` it reads `recomp/perfect.exe`.

**3. Build** for your platform:

| Target | Command | Result |
|---|---|---|
| Linux desktop, Vulkan | `linux/build-desktop.sh` | `build/linux-x64/perfect_recomp`, `build/ddg-recomp-linux-x64.tar.gz` |
| Linux desktop, OpenGL | `RENDERER=gl linux/build-desktop.sh` | same paths |
| ARM handheld, from an x86-64 Linux host | `linux/build-cross.sh` | `build/arm64/ports/`, `build/ddg-recomp-arm64.zip` |
| ARM handheld, from an arm64 host (Apple Silicon, arm64 Linux) | `linux/build.sh` | same as above |
| macOS, Metal | `make -j` (SDL3 from Homebrew) | `perfect_recomp` |

The handheld builds compile in Docker against Debian buster (glibc 2.28) so the binary runs on
old firmware. The first run builds the Docker images, which takes a while. A profile for
profile-guided optimisation (`linux/perfect.profdata`) is used automatically.

**Do not share the built binaries or packages**: they contain the translated game code.

## Running

Desktop:

```sh
build/linux-x64/perfect_recomp "/path/to/game folder"     # the folder with cddata and perfect.exe
```

A settings window (game folder, resolution, aspect, draw distance, fullscreen) opens first. Hold
Shift at start or pass `--settings` to get it back; `--play` skips it. Settings and saves are in
`~/.local/share/TAITO/DengoFinal/` (Linux) or `~/Library/Application Support/TAITO/DengoFinal/`
(macOS). F11 or Alt+Return toggles fullscreen.

Handheld: copy the contents of `build/arm64/ports/` into the SD card's `ports` folder, put
`cddata` and `perfect.exe` into `ports/ddg-recomp/gamedata/`, and restart EmulationStation.
[`recomp/linux/README.txt`](recomp/linux/README.txt) (shipped in the package) covers
installation, controls and the speed settings.

### Controls

| Keyboard | Gamepad | Game |
|---|---|---|
| Arrow keys | D-pad, left stick | menus; ← / → release and apply the brake while driving |
| Return | A, Start | confirm |
| Escape | B, Back/Select | back |
| Space | X | advance tutorial text |
| Page Up / Page Down | L1 / R1 | |
| Home / End | L2 / R2 | |
| F10 | Y | options menu (resolution, draw distance, frame rate, fps counter) |
| | Start + Back | quit |

### Settings (`graphics.ini`)

The original's registry settings plus the features of DDGModernizer, applied at startup without
patching anything:

| Key | Meaning |
|---|---|
| `scrwidth`, `scrheight` | render size; a wider size gives widescreen |
| `aspect` | 3D aspect (`auto` follows the render size) |
| `drawdistance` | `auto` (default): the game's own scenery controller, fed with the measured load (it draws further while frames have time to spare, between `ddmin` 0.45 and `ddmax` 1.0); a number fixes it, as DDGModernizer does (0.9 doubles what is drawn compared with 0.45); `original`: the controller with the original's timing, which keeps it at 0.45 on a PC |
| `fullscreen`, `crop` | borderless fullscreen; show only the game's 4:3 canvas without the overscan margins |
| `maxfps` | draw at most this many frames per second (30, 20, 15; 0 = 60). The game always runs at full speed |
| `frameskip` | skip drawing up to n frames in a row when the machine falls behind, instead of slowing down |
| `texformat` | 1 = 32-bit textures, 0 = paletted as the original |
| `texdump`, `texpack` | write the game's textures out; load HD replacements (see below) |

### HD textures (texture packs)

With the SDL GPU renderer (macOS, Linux Vulkan) and `texformat = 1`, textures can be replaced by
larger versions, for example made with an AI upscaler:

1. Add `texdump = 1` to `graphics.ini` and play: every texture the game shows is written once to
   the `texdump` folder next to `graphics.ini`, as `ddg_<width>x<height>_<hash>.png`.
2. Upscale or edit those PNGs (any size; keep the aspect ratio and the alpha channel) and put them
   into a `textures` folder next to `graphics.ini` (subfolders are fine). The name only has to
   end in the 16-digit hash.
3. Remove `texdump` again. Replacements are loaded in the background when the game first uses a
   texture (the original shows for the few frames until then) and freed once on the GPU;
   `texpackmem` (default 1024) is how many MB of them are kept for reuse. `texpack = 0` turns
   them off.

A texture is recognised by its content, so dumps made with another `texsize` do not match.

Instead of playing everything with `texdump`, all of the game's textures (about 28,000; the HUD
and text textures the game composes at run time still need a dump) can be extracted at once, and
upscaled 4x with [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (`realesrgan-ncnn-vulkan`
from its releases, about an hour on an Apple M-series GPU, about 5 GB):

```sh
cd port && make texpack_extract
./texpack_extract "/path/to/game folder/cddata" /tmp/ddg-textures 2      # 2 = your texsize
cd ../recomp && .venv/bin/pip install pillow numpy
.venv/bin/python tools/texpack_upscale.py /tmp/ddg-textures \
    "$HOME/Library/Application Support/TAITO/DengoFinal/textures/esrgan4x" \
    --esrgan /path/to/realesrgan-ncnn-vulkan
```

Up close the upscaled textures are drawn; from a distance the game's own texels are (they are the
smallest mip level), so far scenery looks as before.

Optionally compress the pack to BC7 (DDS files with their mip levels): it then needs no decoding
and takes a quarter of the memory on the GPU (a Yamanote Line drive peaked at 0.75 GB instead of
1.5 GB). It is larger on disk than the PNGs (3.0 GB instead of 2.1 GB). DDS files win over PNGs
of the same texture, so the PNG folder can be deleted afterwards; GPUs without BC7 use the PNGs.

```sh
.venv/bin/pip install etcpak
.venv/bin/python tools/texpack_bc7.py "$HOME/Library/Application Support/TAITO/DengoFinal/textures/esrgan4x" \
    "$HOME/Library/Application Support/TAITO/DengoFinal/textures/esrgan4x-bc7" --originals /tmp/ddg-textures
```

The OpenGL renderer has more switches (batching, mipmaps, texture stand-ins, an fps counter);
they are listed in `recomp/linux/README.txt`.

## How it works

The PC game is the PS2 game running on a thin "PS2 on Windows" layer that TAITO wrote: the game
logic still builds PS2 GS packets and DMA chains, and the PC layer interprets them with
Direct3D 8. This project keeps the game logic and replaces that layer:

```
PS2 game logic and SDK emulation (x86)  ->  translated to C (recomp/x86lift.py)
PC layer: graphics, textures, GS/DMA/VU1  ->  rewritten natively on SDL3 (port/src/unb/)
Win32, DirectSound, DirectInput, D3DX     ->  shims and native replacements (recomp/host/)
```

- **Translator** (`recomp/x86lift.py`, `build_game.py`): disassembles with capstone and emits one
  C function per x86 function, operating on a flat 32-bit guest memory and an emulated stack, so
  pointers stored in game data keep working. x87 floating point becomes double arithmetic
  on a modelled register stack; small leaf functions are inlined; callee-saved registers and
  the FPU stack are kept in locals where that can be proven. `recomp/data/` holds the function
  lists (addresses and sizes) exported from Ghidra that guide the translation.
- **Runtime** (`recomp/host/`): PE loader, guest memory, address dispatch for indirect calls,
  guest threads (the game's PS2 threads on top of Win32 events), the Win32 API subset the game
  uses, a DirectSound mixer on SDL audio, input, the settings window.
- **Port layer** (`port/src/unb/`): the GS register emulation, DMA/VIF1 interpretation, the VU1
  replacement for 3D models, PS2 texture decoding, and two renderers (`Graphic.cpp` on SDL GPU,
  `GraphicGL.cpp` on OpenGL / GLES with a render thread). See [`port/README.md`](port/README.md).
- **Verification** (`recomp/verify.py`): runs original functions in Unicorn and their
  translations from the same random state, and compares registers and memory.

[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) has the full notes: architecture details,
everything found and fixed along the way, performance measurements, the debugging environment
variables and known issues. [`docs/TASKS.md`](docs/TASKS.md) maps the game logic: its tasks and
modes, the driving loop, the train's variables and the power/brake controls.
[`notes/game-structure.md`](notes/game-structure.md) describes the executable's layout.

## Repository layout

| Path | Contents |
|---|---|
| `recomp/` | translator, runtime, build scripts; `recomp/linux/` the Linux and handheld builds |
| `recomp/data/` | function lists from Ghidra (addresses, sizes, call graph) used by the translator |
| `recomp/build/discovered.txt` | function entries found at run time (an input, kept in git) |
| `recomp/tools/` | benchmark, profiling and screenshot helpers |
| `port/` | the rewritten PC layer, standalone demos and tools (`txm2png` decodes game textures) |
| `scripts/` | Ghidra scripts used for the analysis (headless) |
| `include/` | Direct3D 8 declarations and recovered types for Ghidra |
| `docs/`, `notes/` | development notes |

## Legal

ddg-recomp is not affiliated with or endorsed by TAITO or Square Enix. *Densha de GO!* is a
trademark of TAITO Corporation.

The repository contains only original code and documentation: the translator, the runtime, the
rewritten port layer, scripts, and lists of function addresses. It contains no part of the
game's code or data. The translated game code is generated locally from an executable you own;
binaries built from it contain that code and must not be distributed.

The code in this repository is licensed under the [GNU General Public License v3.0](LICENSE).
Most of it was generated by AI (see the notice at the top); the legal status of AI-generated code
is not settled in every country.
The handheld and desktop packages bundle SDL3, which is under the zlib licence.

## Credits

- [DDGModernizer](https://github.com/hunterbridges/DDGModernizer) (MIT licence), whose patch
  list for FINAL (`recomp/data/ddgmodernizer-final.txt`, licence in `ddgmodernizer-LICENSE.md`)
  this project applies and whose enhancements it offers as settings
- [SDL](https://www.libsdl.org/), [capstone](https://www.capstone-engine.org/),
  [Unicorn](https://www.unicorn-engine.org/), [Ghidra](https://ghidra-sre.org/)
