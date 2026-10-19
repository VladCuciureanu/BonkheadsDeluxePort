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

### Windows

You need Visual Studio 2022 or 2026 with the "Desktop development with C++"
workload, which includes CMake and Ninja. Python comes from python.org or the
Microsoft Store.

`build.bat` sets up the VS 2026 x64 environment, then configures and builds
into `build/`. Extra arguments go to CMake. For another Visual Studio version,
run the CMake steps yourself from an "x64 Native Tools Command Prompt":

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

### macOS

Install the Xcode command-line tools (`xcode-select --install`), then CMake
and, optionally, Ninja:

```bash
brew install cmake ninja
```

```bash
./build.sh
```

### Linux

Install a compiler, CMake, Python 3 and SDL3's build dependencies. On
Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake ninja-build python3 pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libdrm-dev libgbm-dev
```

```bash
./build.sh
```

Text uses Noto Sans or DejaVu Sans if one is installed (`fonts-noto-core`
or `fonts-dejavu-core`).

`build.sh` uses Ninja when it is installed and passes extra arguments to
CMake, for example `./build.sh -DBONKHEADS_SIT=$HOME/Bonkheads_Deluxe.SIT`.
Set `BUILD_DIR` or `BUILD_TYPE` (default `RelWithDebInfo`) to change the
output folder or configuration.

The game ends up in `build/` (`bonkheads.exe` on Windows, `bonkheads` on
macOS/Linux), with its data in `build/data/`.

### Building without the game (CI)

`-DBONKHEADS_STUB_GAME=ON` builds the runtime, Toolbox and platform code
against an empty stub (`tools/stub/recomp_table.c`) instead of the
recompiled game. It doesn't need the archive or Python. The result only runs
`--help`, but it shows the port compiles and links. `-DBONKHEADS_WERROR=ON`
turns warnings in the port sources into errors.

```bash
./build.sh -DBONKHEADS_STUB_GAME=ON -DBONKHEADS_WERROR=ON
```

The tools have unit tests that need no game data:

```bash
python -m unittest discover -s tools/tests
```

GitHub Actions (`.github/workflows/ci.yml`) runs both on Windows, Linux and
macOS.

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
the Mac. The mouse pointer shows in menus and dialogs, and hides during play
as the game requests. Saved games and preferences go in the game data folder.

### Internet multiplayer

Hover over a menu button to see what it does.

- **NET HOST** hosts a game. While you wait, a banner shows your LAN address
  and the port to forward. On the internet, give your friend your public IP
  and forward UDP port 24731 to your computer (change it with `--port`).
  When your friend shows up in the "Where's your brother?" list, pick them
  and click OK.
- **NET JOIN** joins a friend who chose NET HOST. Type the address shown on
  their screen, for example `203.0.113.7` or `192.168.1.20`. It's remembered
  for next time. Leave it empty to find a host on your local network. The
  joining side dials out, so only the host needs to forward a port.

For scripts, `--connect HOST[:PORT]` gives NET JOIN an address and skips the prompt.

The transport layer adds two things on top of the original protocol, to cope
with real internet links:

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

- **Running on macOS and Linux.** The port builds on both (Linux with GCC
  and Clang, macOS in CI), but the game has only been played on Windows.
- **The whole game played through.** Later worlds, bosses, save/load,
  high-score entry and the ending have not been tested yet.
- **The high-score panel on the main menu,** which currently stays black.

**Text and fonts:**

- Text drawn on screen is rendered at the window's full resolution. It uses
  the OS interface font: Segoe UI on Windows, the system font on macOS, and
  Noto Sans or DejaVu Sans on Linux. These stand in for the classic Mac
  Chicago, Geneva and Helvetica.
- The game's own "Thwack" font is rendered from its embedded TrueType data.
- Text the game draws into offscreen buffers is anti-aliased into its
  256-color palette.
- `--pixel-text` restores the old low-resolution text.

**Approximations:**

- QuickDraw regions are bounding rectangles.
- Round rects and ovals draw as rectangles.

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
