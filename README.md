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
- On the R36H (RK3326, 4× Cortex-A35 at 1.5 GHz, Mali-G31), driving on the Yamanote Line with
  the PC's draw distance (0.9) runs at about 60 frames/s (`maxfps = 0`, `vsync = 0`), or a
  steady 30 with `maxfps = 30` (less heat and battery use). The shipped handheld settings use
  draw distance 0.4.

## What you need

- The PC version of Densha de GO! FINAL, installed: the `cddata` folder (about 2.6 GB) and an
  **unprotected `perfect.exe`**. The `perfect.exe` on the CD is protected by SafeDisc (its code
  is encrypted) and cannot be used (SafeDisc does not run on Windows 10 and later either, as its
  driver is disabled there). A copy patched by
  [DDGModernizer](https://github.com/hunterbridges/DDGModernizer) works as well: its patches
  are applied by this project anyway, and its settings are options in `graphics.ini`.
- To build: either **Docker** alone, or Python 3 with [capstone](https://www.capstone-engine.org/)
  5.0, clang, make and SDL 3.2 or newer.

The recompiled code is generated from your `perfect.exe`, and the game checks at startup that it
runs with the same exe (it reads the game's tables and texts from it). Other versions, or an exe
with a translation patch, need their own build.

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
| | Start + Back | quit |

### Settings (`graphics.ini`)

The original's registry settings plus the features of DDGModernizer, applied at startup without
patching anything:

| Key | Meaning |
|---|---|
| `scrwidth`, `scrheight` | render size; a wider size gives widescreen |
| `aspect` | 3D aspect (`auto` follows the render size) |
| `drawdistance` | scenery distance (0.9 is the PC original) |
| `fullscreen`, `crop` | borderless fullscreen; show only the game's 4:3 canvas without the overscan margins |
| `maxfps` | draw at most this many frames per second (30, 20, 15; 0 = 60). The game always runs at full speed |
| `frameskip` | skip drawing up to n frames in a row when the machine falls behind, instead of slowing down |
| `texformat` | 1 = 32-bit textures, 0 = paletted as the original |

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
