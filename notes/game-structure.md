# Game structure survey (perfect.exe, Densha de GO! FINAL PC)

## Program flow
- CRT entry 0x4a45af -> WinMain 0x407260: parse args, load settings (0x407170, CGraphic 0x402110,
  input 0x4055a0), create window (0x406f70), CGraphic::Init, sound 0x409060, input 0x4056d0,
  movie 0x406660, helper thread 0x401430, then **PS2 main() at 0x478ed0** called as main(0, 0).
- 0x478ed0 is the unchanged PS2 main: sceSifInitRpc, loads IOP modules from cddata/module
  (no-ops on PC), sceGsResetGraph(640, 224, ...) at 0x419100, then starts the game thread
  0x479200 through the PS2 thread emulation (0x41ad00, built on Windows events) and waits.
- One frame = one DMA send on channel 1 (game side 0x419da0 -> sceDmaSend 0x4145b0).

## Ghidra coverage
The first auto-analysis missed ~190 KB of code. scripts/RecoverCode.java disassembles all direct
call targets and orphaned instruction runs: functions 3,154 -> 4,541; code inside functions
495 KB -> 655 KB; ~40 KB left undefined (jump tables / constants). Backup of the project before
this step: proj.bak-before-recover.

## Regions (after recovery)
| Region | Functions | Code | Reachable from main |
|---|---|---|---|
| 0x401000-0x415000 PC layer (unb) | 350 | 77 KB | 298 |
| 0x415000-0x41d000 PS2 SDK emulation (+VU0 math, CRI file I/O) | 355 | 30 KB | 326 |
| 0x41d000-~0x4837xx game (PS2 code) | 2,359 | 366 KB | 1,837 (284 KB, 86,808 instr) |
| ~0x484000-0x4b0000 libraries (D3DX, CRT, DXErr, libjpeg) | 1,245 | 182 KB | 1,169 |

Note: the survey's "lib" bucket starts at 0x480000; 0x480000-0x4837xx is actually game code.

Reachable game code: 3.9 % x87 instructions, 44 indirect calls, 97 indirect jumps (86 functions
with switch tables). Reachability = direct calls + any 32-bit value in the image equal to a
function entry (function pointers).

## Boundary (what game code calls outside itself): 269 functions, 18.5 KB total
- 197 of them are <= 64 bytes. Mostly PS2 SDK shims (sce* stubs, thread/semaphore emulation
  0x41ad00-0x41b7xx, file I/O incl. criopfile.c 0x41ba20.., VU0 math wrappers 0x4156xx-0x4159xx
  over D3DX), PC-layer entry points (0x4074a0 message pump/vsync, 0x4078xx-0x407dxx small
  wrappers incl. Movie), and a few CRT helpers (__ftol, __allshl, _rand, sprintf-like 0x4a2a50).
- Files: boundary.tsv (entry, game call sites, name, size, imports, strings).

## Function-pointer tables
tables.tsv: 57 runs of >= 2 function pointers in data; 10 point into game code (state machines).
