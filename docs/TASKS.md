# Game logic map — tasks, modes, driving and controls

The game logic is the PS2 game's, running on TAITO's PS2 emulation layer. It is not one loop but
a set of **cooperative tasks** in a priority-ordered list. This page describes what is known so
far (2026-10-13), found by reading the code and by experiments on the running game (switching
tasks and functions off, memory dumps). Addresses refer to `perfect.exe`; ids and priorities are
hex, as the game itself names tasks (`TASK:%02X:%02X`).

Contents: [scheduler](#the-scheduler-0x41ab00-0x41af70) · [tasks](#the-tasks) ·
[game modes](#game-modes-scene-manager-0x4824a0) · [driving mode](#the-driving-mode-0x44e500) ·
[controls: power and brake notch](#controls-power-and-brake-notch) · [tools](#tools) ·
[next steps](#next-steps)

## The scheduler (0x41ab00-0x41af70)

| Address | Function | Notes |
|---|---|---|
| `0x41acc0` | init | empty list, creates the main event `0x678f00` |
| `0x41ad00` | `create(id, entry, arg, stack, prio \| wait << 8)` | allocates the 0x58-byte task, inserts it by priority, starts a Windows thread at `0x41ab50` |
| `0x41ab50` | thread start | waits for the first turn, calls `entry(arg)`, then `exit` |
| `0x41ab70` | `dispatch(from)` | from task `from` on: counts the wait counters down and wakes the first task that reaches 0, then blocks the caller until it is woken again; when none is ready it signals the main event (back to the message loop) |
| `0x41ab00` | wait for the turn | `WaitForSingleObject` on the task's event (or the main event for the message loop) |
| `0x41af10` | **vsync tick** | `dispatch(head)`; called once per message-loop pass (`host/native.cpp`, 60 Hz) |
| `0x41af20` | `sleep(n)` | sets the current task's wait counter to n and dispatches from the next task |
| `0x41ae30` | `control(id, op)` | op 0 kill, 1 suspend, 2 resume; id -1 = all but the current task |
| `0x41aef0` | exit | unlinks the current task and dispatches |
| `0x41af50` | find by id | |

So one tick runs every ready task once, in priority order (low number first); a task that sleeps
n ticks runs every n-th tick.

**In the port** the scheduler is native (`recomp/host/tasks.cpp`, since 2026-10-13): every task is
a fiber on the message loop's thread instead of a Windows thread, and a hand-over is a register
swap instead of SetEvent + WaitForSingleObject. The task list and structures stay in guest memory
as above; the C runtime's per-thread data is still allocated, set up and freed as `_beginthread`
/ `_threadstart` / `_endthread` did. Globals: list head `0x758208`, current task `0x758204`.

**Task structure** (0x58 bytes, `0x41ad00`):

| Offset | Field |
|---|---|
| `+0x00` | flags (1 started?, 4 re-armed every frame when `0x678f08` is set, 8 finished) |
| `+0x01` | priority (list order) |
| `+0x04` | id (-1 at creation = next free negative id) |
| `+0x08` | wait counter (u16), `+0x0a` suspend count (u16) |
| `+0x0c` / `+0x10` | next / previous |
| `+0x1c` | argument |
| `+0x20` | address right after the 0x58-byte structure (use unknown) |
| `+0x24` | **entry function** |
| `+0x28` | thread handle, `+0x2c` wake-up event |
| `+0x30` | name `TASK:id:prio` |

## The tasks

Measured on the PC (Osaka Loop drive, `RECOMP_TRACE_TASKS`): 13 tasks, all sleeping 1 tick, so
each runs every tick. "Suspended" = what happens when the task is held from second 75 (driving)
or 45 (loading the route) with `RECOMP_SUSPEND_TASK`.

| id | prio | entry | created by | CPU ms/tick (driving) | Role | Evidence | Confidence |
|---|---|---|---|---|---|---|---|
| 00 | 00 | `0x479200` | PS2 main `0x478ed0` | 0.83 | **Frame driver**: flips and sends the frame (`0x419270`, where the DMA chain and our packet path run), swaps the double buffers, prepares the next frame's packet buffers (`0x47e820/8a0/860`), counts frames in `0x74f57c`; creates the scene manager | code | high |
| 50 | 01 | `0x41bf10` | `0x41b8fa` | 0.02 | **File loading** (request side?); calls `0x46a520`, reaches the sound code | suspended while loading: the route never finishes loading (160 textures instead of ~1100) | medium |
| 33 | 20 | `0x417790` | `0x4176b2` | 0.05 | **Frame synchronisation** (vsync/DMA-completion handler of the PS2 layer) | suspended: black screen, 16-18 ticks/s | medium |
| 51 | 21 | `0x41bfc0` | `0x41b90f` | 0.02 | **File loading** (read server?) | as id 50 | medium |
| fa | 22 | `0x41b3a0` | `0x41b2c7` | 0.02 | file I/O and texture code; no effect while loading or driving | reach | low |
| 64 | 40 | `0x4824a0` | frame driver | 0.02 | **Scene manager**: initialises the subsystems (`0x482580`, which creates most other tasks), then switches game modes (below) | code | high |
| 63 | 41 | mode entry | scene manager | 1.14 (driving) | **Current game mode** (driving `0x44e500`, menus `0x456b60`, ...) | code, trace | high |
| 41 | 60 | `0x474b00` | `0x474af2` (`0x474ab0`) | 0.03 | walks a table at `0x72bb08..0x72bb0c` calling `0x42e390`; no visible effect | code | low |
| 2d | 85 | `0x44be30` | `0x44ba84` | 0.03 | reaches texture code; no visible effect | reach | low |
| 2f | f4 | `0x479d90` | `0x479c2c` (`0x479be0`) | 0.07 | updates a pool of 0x128-byte objects (`0x72c9e8..0x72c9e4`, `0x47a8e0` active?, `0x47a1a0` update); no visible effect on the Osaka Loop | code | low |
| 30 | f5 | `0x41e0f0` | `0x41e051` (`0x41e020`) | 0.39 | **2D sprite system / HUD**: 10 lists at `0x714700` (12 bytes each), nodes of type 1 (`0x41e220`) and 2 (`0x41e4d0`) | suspended: the whole HUD disappears, the 3D scene stays | high |
| 31 | f6 | `0x41b7e0` | `0x41b69c` (`0x41b660`) | 0.02 | **Timers / blink counters** (probably) | suspended: HUD lamps and map markers freeze in one state | medium |
| 33 | ff | `0x4177f0` | `0x4176c9` | 0.03 | **Frame finish** (runs last every tick) | suspended: only the sky is drawn, rest of the frame black, no HUD | medium |

Tasks 0x41, 0x2d, 0x2f and 0xfa had no effect on the Osaka Loop drive, its loading or its sound;
they belong to situations not covered yet (menus, saving, other routes, events).

## Game modes (scene manager `0x4824a0`)

The scene manager checks the requested mode (`0x73505c`) every tick. On a change it kills task
0x63, calls the old mode's exit function and starts the new mode's entry as task 0x63 (priority
0x41). Current mode `0x735058`, previous `0x735060`. Table at `0x5e3178`, 12 bytes per mode
(stack size, entry, exit):

| Mode | Entry | Exit | Meaning |
|---|---|---|---|
| 1 | `0x46a900` | – | start-up? |
| 2 | `0x44e500` | `0x44e9b0` | **driving** (stack 0x6000) |
| 3 | `0x456b60` | `0x456ca0` | **title and menus** |
| 4 | `0x426040` | `0x4260a0` | unknown |
| 9 | – | – | quit (`jmp 0x4077d0`) |
| 10, 11 | `0x482cd0`, `0x482f20` | `0x482e30`, `0x4830b0` | stack fields 0xb / 0xf: probably not the same table |

## The driving mode (`0x44e500`)

Once per tick (task 0x63), after a set-up part (`0x44e840`, `0x4551b0`, `0x451390`, `0x455290`,
`0x4549f0`, `0x4552d0`, `0x451510`, `0x454770(2)`):

1. `0x44e620`, `0x479770`, `0x430940` (returns 4: `0x44e7b0`, else `0x451760`), `0x44e950`,
   `0x453f10`; then `0x451740` decides whether the rest runs this tick (0: sleep, start over).
2. 21 calls from `0x427bd0` to `0x47f520` (below), `[0x75733c] = [0x757338]`, sleep 1 tick.

Each function was switched off from second 75 of the Osaka Loop drive (`LIFT_SKIPPABLE` +
`RECOMP_SKIP`, below) and the result compared with a reference run: screenshot at 88 s, game
clock, sound level and memory dumps of the train variables.

| Function | Switched off | Role |
|---|---|---|
| `0x479770` | notch stays N, no speed, train stands, motor sound gone | **controls**: driver's power/brake input |
| `0x427bd0` | speed stays 0, train stands (notch 5 shown), quieter | **train physics**: speed from power, brakes, gradients |
| `0x456900` | speed rises as normal, but the position stays | **movement**: advances the train along the route by its speed |
| `0x446490` | distance correct, but the scenery stays where the train was | **camera / view position** |
| `0x451350` | game clock stops, displayed speed (`0x757080`) stays 0, train moves | **game time** and derived values (reaches the speed calculation `0x4498f0`) |
| `0x425290` | a train on the next track stays standing, louder | **other trains** |
| `0x43df90` | the whole HUD freezes (clock, distance, speed, notch) | **HUD values** from the game state |
| `0x47d940` | the whole 3D scene disappears, HUD stays | **3D scene** (includes the object loop `0x47c8c0` of the performance work) |
| `0x44e620`, `0x44e7b0`, `0x451760`, `0x44e950`, `0x453f10`, `0x454360`, `0x456460`, `0x454e10`, `0x4374e0`, `0x44d150`, `0x433f20`, `0x433a80`, `0x42f300`, `0x4313a0`, `0x443950`, `0x483590`, `0x47f190`, `0x47f030`, `0x47f520` | no visible or audible change in 13 s of this drive | situational (signals, stops, scoring, ATS, weather, ...) or shared with the menus (`0x483590`, `0x47f190`, `0x47f030`, `0x47f520` are called by the menu mode too) |

**Train variables** (found by searching memory dumps at 74, 81 and 88 s for the HUD values;
floats unless noted):

| Address | Meaning |
|---|---|
| `0x74f604` (copy `0x716d04`) | speed in metres per tick (physics) |
| `0x757080` (copies `0x7570c0`, `0x716d2c`) | speed in km/h (speedometer), written by `0x4498f0` |
| `0x751800` | position along the route, m |
| `0x751784` / `0x751788` / `0x751790` | world position y / x / z |
| `0x7517b8` (copies up to `0x751850`) | distance to the next stop, m; `0x751654` the same in cm (int), `0x74f4c8` in m (int, HUD) |
| `0x751780` | start of this block (argument of `0x454360`) |

## Controls: power and brake notch

**How they were found:** on the Osaka Loop drive, scripted keys stepped the brake from emergency
to released (14 presses of Left), power from N to P5 (5 x Down) and back (5 x Up), and the brake
to B3 (3 x Right). A memory dump half a second after every step (28 dumps, `RECOMP_MEMDUMP`) was
searched for 8-, 16- and 32-bit values and floats that follow the expected sequence; exactly the
locations below match, and reading the code confirmed which are the originals (written by the
input code, read in ~30 places) and which are copies.

| Address | Meaning |
|---|---|
| `0x751a04` | **power notch**, int: 0 = neutral (N), 1..5 = P1..P5 (maximum in `0x72c9d4`) |
| `0x751a00` | **brake notch**, int: 0 = released, 1..8 = B1..B8, 9 = emergency (maximum in `0x72c9c8`) |
| `0x72c9d8` / `0x72c9c4` | the previous tick's power / brake (kept by `0x479770`) |
| `0x7570bc` / `0x7570b8` | copies in the state record next to the speed (`0x4498f0`) |
| `0x718cd0`/`0x718ce4`, `0x719700`/`0x719704`, `0x71a610`/`0x71a614` | further copies (`0x433470`, `0x440d10`, `0x44a9a0`; HUD and sound, probably) |

The controls function `0x479770` sets the notches through `0x479600(power)` and
`0x479630(brake)`. Overrides first: if `0x479b90()` returns 1, power 0 and brake -1 (forced
stop); if `0x479bd0()` returns 1, power 0 and brake `0x479710()` (automatic braking). Otherwise
`0x72c868` selects the control mode: 0 normal input (`0x4798c0`), 1 none (notches kept), 2 and 3
the notches limited to the maxima, 4 only records the current notches as the previous ones
(with the previous power set to 5; purpose unknown).

`0x4798c0` reads the game's pad bits, `0x72c864` (held, with repeat) and `0x72c87c` (newly
pressed), in one of two schemes chosen by `0x72ca34` (`[0x72c9f0 + 0x44]`):

- **Two handles** (0 or 1), masks per scheme in a table at `0x5dafd8` (7 dwords): power +1 and
  -1 (held, only while the brake is released), power to neutral (pressed), brake +1 (held; to
  emergency only with a new press), brake -1 (held), full service brake B8 (pressed), brake
  released (pressed). Scheme 0: `4000 1000 0004 0080 0040 0010 0008`; scheme 1:
  `0040 0080 0008 8000 4000 1000 0004`.
- **One handle** (other values): one lever value from -brake to +power; bit `0x4000` moves it
  towards power, `0x1000` towards brake, bits `0x000c` set it to neutral.

Keyboard (scheme 0, PC key mapping `0x40f5e0`): Down power +1, Up power -1, Right brake +1
(held: up to B8), Left brake -1, Home brake released. A train controller can be supported by
producing these pad bits (or by writing the two notch variables directly, as the controller's
absolute positions).

**Open questions:**
- Which key or button produces which pad bit is not confirmed: in the dumps `0x72c864` was
  mostly 0 even while a key was held (it seems to carry per-tick pulses for the repeat, used up
  before the dump). The key effects above are observed, not derived from the bits. Logging
  `0x72c864`/`0x72c87c` every tick would settle it.
- What sets the scheme selector `0x72ca34` (an option in the game's settings?) and which
  physical controllers the two schemes stand for (TAITO's PS2 controllers report as pads).
- The forced-stop and automatic-braking overrides (`0x479b90`, `0x479bd0`, `0x479710`):
  probably ATS and the end of a run.

## Tools

- `RECOMP_TRACE_TASKS=1` (with `RECOMP_TRACE_TICKS`): every 10 s the task list with id,
  priority, entry, argument, wait, suspend count, flags and CPU time per tick (Linux).
- `RECOMP_SUSPEND_TASK=<entry>@<s>[,...]`: keep the task with that entry suspended from tick time
  s on. Switching tasks off one at a time is the quickest way to find out what they do.
- `LIFT_SKIPPABLE=<addr> ...` (translator, at `build_game.py` time) + `RECOMP_SKIP=<addr>@<s>[,...]`
  (run time): the listed functions return at once from tick time s on. Without `LIFT_SKIPPABLE`
  the generated code is unchanged.
- `RECOMP_MEMDUMP=<s>,...`: memory dumps (`build/mem<n>.bin`: 0x4b2000-0x860000, then 32 MB of
  heap from 0x10000000) for finding variables by their values.
- `recomp/tools/retool.py dis|xref|calls|profile|reach` (static: disassembly, references,
  call-site arguments, callees/strings/globals, reachable PC-layer modules).
- `scripts/ExportNames.java`: names from a Ghidra project (`RE_NAMES` for `retool.py`).

## Next steps

- Driving mode: signals and ATS, the timetable and scoring; the 19
  functions without a visible effect need situations where they matter (stations, signals,
  overruns), and the structures at `0x757000`, `0x751780` and `0x716d00` their remaining fields.
- Find the situations of tasks 0x41, 0x2d, 0x2f, 0xfa (menu runs, saving, another route).
- Name the frame-related SDK functions (`0x419270`, `0x4171c0/1e0`, `0x417860/870`).
