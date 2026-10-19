# Bonkheads Deluxe: cross-platform port

This is a source port of **Bonkheads Deluxe** (1 A.M. Productions, 1997), a
classic Mac OS 68k game. It runs on Windows, macOS and Linux using SDL3, and
its original AppleTalk network mode works over the internet via UDP.

**Bring your own copy.** This repository holds no game code, graphics,
sound or levels. At build time the tools read your copy of
`Bonkheads_Deluxe.SIT` and do two things:

1. Extract the archive.
2. Recompile the game's 68k machine code into C.

The generated code and extracted data stay in your build folder.

## How it works

```
Bonkheads_Deluxe.SIT ──tools/sit5.py──▶ resource forks (code, PICT, snd, Sprt, levels)
        CODE resources ──tools/recomp.py──▶ build/gen/recomp_*.c  (one C function per original function)
                                             │
            src/runtime  (68k register file, big-endian memory, dispatch)
            src/toolbox  (Mac Toolbox reimplemented: QuickDraw, Resource/Memory/File/
                          Dialog/Window/Event/Sound Managers, AppleTalk → UDP)
            src/platform (SDL3 video/input/audio, UDP sockets)
```

### Tools

- **`tools/sit5.py`** extracts StuffIt 5 archives. It is a clean-room decoder
  for the "Arsenic" compression method (BWT plus arithmetic coding).
- **`tools/m68k.py`, `tools/dumpcode.py`** form a 68000/68020 disassembler. It
  uses the original MacsBug function names and resolves the CodeWarrior
  far-model jump table and relocations.
- **`tools/recomp.py`** is a static recompiler from 68k to C. Its main steps:
  - Every original function becomes a C function that keeps its original
    name (`f_Player1Processor`, `f_Boss1Brain`, …).
  - Flag liveness analysis avoids computing condition codes that are never
    read.
  - Switch tables are resolved statically.

### Toolbox emulation

`src/toolbox` re-implements, in C, about 300 Mac Toolbox and OS calls that
the game uses. Graphics come out pixel-exact because:

- the game's own sprite blitters write directly to an emulated 8-bit
  framebuffer;
- a separate "hardware" palette is only changed by `SetEntries`, which is
  what makes the original fades work.

### Design notes

See `docs/` for the design notes:

- `docs/hle-spec.md` covers what the game expects from the Toolbox.
- `docs/network.md` covers the network layer.

## Building

You need:

- CMake ≥ 3.20
- a C11 compiler
- Python ≥ 3.8
- internet access on the first configure, to download SDL3 and stb_truetype

Put your `Bonkheads_Deluxe.SIT` in the repository root, or pass
`-DBONKHEADS_SIT=/path/to/it`.

**Windows** (Visual Studio 2022/2026 with C++ tools): run `build.bat`. To run
the CMake steps yourself from a developer prompt:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

**macOS / Linux:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j
```

On Linux, install the SDL3 build dependencies first. On Debian/Ubuntu those
are `libx11-dev`, `libxext-dev`, `libwayland-dev`, `libasound2-dev`,
`libpulse-dev` and so on.

The game ends up in `build/` (`bonkheads.exe` on Windows, `bonkheads` on
macOS/Linux), with its data in `build/data/`.

## Playing

```bash
./build/bonkheads
```

Optional flags:

| Flag | Effect |
|---|---|
| `--scale N` | Window scale factor |
| `--fullscreen` | Start in fullscreen; Alt+Enter toggles it |
| `--nosound` | Disable audio |

Controls are the original defaults:

| Player | Left | Jump | Right |
|---|---|---|---|
| Player 1 | ← | ↑ | → |
| Player 2 | Control | Option (Alt) | Command (Windows/Super key) |

You can rebind keys in **Preferences → Keys**. Caps Lock pauses, as it did on
the Mac. Saved games and preferences go in the game data folder.

### Internet multiplayer

The original game's roles are kept, and so is its whole network protocol:

- **Net Join** waits for a partner. This player needs a reachable UDP port:
  24731 by default, changed with `--port`. Forward it on your router if
  you're behind NAT.
- **Net Host** looks for joiners and runs the game simulation. Point it at
  the joiner's address:

```bash
bonkheads --connect friend.example.com:24731
```

Players on the same LAN find each other automatically via broadcast. The
joiner then appears in the host's "Where's your brother?" list. Pick it and
both players continue as in 1997.

To cope with real internet links, the transport layer adds two things on top
of the original protocol:

- **Pacing.** Each game packet is held until the game has consumed the
  previous one, so packet jitter doesn't overwrite unprocessed state. Tune
  this with `--hold N`.
- **Redundancy.** Every game packet is sent twice, to absorb loss. Disable
  this with `--no-redundancy`.

`--raw-net` turns both off. Testing used `--sim-latency MS` and
`--sim-loss PCT`. A session played smoothly through about 120 ms of round
trip with 5% packet loss.

## Status and known limitations

**Verified on Windows 11:**

- intro screens with palette fades
- main menu, preferences and name dialogs
- the Worlds screen and level 1 gameplay
- a full two-instance network game:
  - discovery and handshake
  - level and character-map transfer
  - synchronized play
- the network game under simulated latency and loss

**Not yet verified:**

- **macOS and Linux builds.** The code only uses SDL3, standard C and BSD
  sockets, but the build has only been run on Windows so far.
- **The whole game played through.** Later worlds, bosses, save/load,
  high-score entry and the ending have not been tested yet.
- **The high-score panel on the main menu,** which currently stays black.

**Approximations:**

- QuickDraw regions are bounding rectangles.
- Round rects and ovals draw as rectangles.
- The classic Mac system fonts (Chicago, Geneva, Helvetica) are replaced
  by host TrueType fonts. The game's own "Thwack" font is rendered from
  its embedded TrueType data.

**Not ported:** the level editor ("The Bonkreator").

## Debugging aids

| Flag | Effect |
|---|---|
| `--trace` | Log every Toolbox call |
| `--netlog` | Log reliable network events |
| `--shot SECONDS FILE.bmp` | Save a screenshot and exit |
| `--press SECONDS:MACKEY[:FRAMES]` | Scripted key press |
| `--click SECONDS:X:Y` | Scripted mouse click |

`tools/shot.ps1` wraps these for automated runs on Windows.
