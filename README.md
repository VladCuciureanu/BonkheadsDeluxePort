# Bonkheads Deluxe: cross-platform port

A source port of **Bonkheads Deluxe** (1 A.M. Productions, 1997), a two-player
platform game for the classic 68k Mac. It runs natively on Windows, macOS and
Linux on top of SDL3. The original AppleTalk network mode works over the
internet through UDP.

**Bring your own copy.** This repository contains no game code, graphics,
sound or levels. The build reads your copy of `Bonkheads_Deluxe.SIT` and does
two things:

1. Extracts the archive.
2. Recompiles the game's 68k machine code into C.

Everything derived from the game stays in your local `build/` folder, which
git ignores.

## Quick start

1. **Install the tools** for your system (details in [Building](#building)):

   | System | What to install |
   |---|---|
   | Windows | [Visual Studio 2022 or newer](https://visualstudio.microsoft.com/downloads/) (Community is free) with the **Desktop development with C++** workload, plus [Python 3](https://www.python.org/downloads/) |
   | macOS | `xcode-select --install`, then `brew install cmake ninja python` |
   | Linux | a C compiler, CMake, Python 3 and SDL3's build dependencies ([list](#linux)) |

2. **Copy `Bonkheads_Deluxe.SIT`** into this folder, next to this README. Any
   `.sit` name works as long as it's the only one there.

3. **Build.** On Windows, double-click `build.bat` or run it in a terminal:

   ```bash
   build.bat
   ```

   On macOS and Linux:

   ```bash
   ./build.sh
   ```

   The first build downloads SDL3 and takes a few minutes. Later builds take
   seconds.

4. **Play.** Run `build\bonkheads.exe` on Windows or `./build/bonkheads` on
   macOS and Linux.

## Playing

Controls are the original defaults:

| Player | Left | Jump | Right |
|---|---|---|---|
| Player 1 | ← | ↑ | → |
| Player 2 | Control | Option (Alt) | Command (Windows / Super key) |

Other keys and features:

- Rebind the controls in **Preferences → Player Controls**.
- **Caps Lock** pauses the game, as it did on the Mac.
- **Alt+Enter** toggles fullscreen.
- **QUIT** on the main menu exits.
- Hover over a main-menu button to see what it does.

Options:

| Flag | Effect |
|---|---|
| `--scale N` | Window scale factor (default 2). The window can also be resized freely. |
| `--fullscreen` | Start in fullscreen. |
| `--nosound` | Disable audio. |
| `--pixel-text` | Draw text in the original low-resolution bitmap fonts. |
| `--data DIR` | Use game files from another folder. |

`bonkheads --help` lists every option.

### Where your data lives

Preferences, high scores and saved games go into the game data folder,
`build/data/Bonkheads Deluxe/`, just as the original kept them next to the
application. The relevant files:

| File | Contents |
|---|---|
| `Bonkheads Deluxe Prefs.rsrc` | Settings and both high-score tables. Delete it, and its `.finf` file, to reset them. |
| `network.cfg` | The last host address typed into NET JOIN. |
| `highscores.cfg` | Optional. Extra entries for the default high-score table (see below). |

The port stores Mac files as host files plus two sidecars:

- `NAME`: the data fork.
- `NAME.rsrc`: the resource fork.
- `NAME.finf`: the Finder type and creator.

Rebuilding never overwrites your preferences or saves.

To add your own names to the default high-score table, create
`highscores.cfg` in the data folder with one `name, score, level` entry per
line:

```
# name, score, level
Alice, 13337, 10
```

Each entry is inserted where its score ranks. The file only affects a fresh
table, so delete the prefs file to see it.

## Multiplayer over the internet

One player hosts and the other joins. Only the host needs a reachable port.

**Host (NET HOST):**

1. Click **NET HOST**. A banner shows your local address and the port to
   forward.
2. Tell your friend your address:
   - On the same network, use the local address from the banner, for
     example `192.168.1.20`.
   - Over the internet, use your public IP address (search "what is my
     IP"). Forward **UDP port 24731** on your router to your computer, and
     allow the game through your firewall.
3. When your friend appears in the "Where's your brother?" list, select
   their name and click **OK**.

**Joiner (NET JOIN):**

1. Click **NET JOIN**.
2. Type the host's address, for example `203.0.113.7` or `203.0.113.7:5000`.
   The address is remembered for next time.
   - If you leave it empty, the game looks for a host on your local network.
   - You don't need to forward anything: the game dials out to the host.

Tips:

- **Other port.** The host can use another port with `--port N`. The joiner
  then types `ADDRESS:N`.
- **Skip the prompt.** `--connect HOST[:PORT]` gives NET JOIN its address and
  skips the prompt.
- **No router access.** If neither player can forward a port, a VPN that
  puts both computers on one virtual LAN works, for example
  [Tailscale](https://tailscale.com/) or [ZeroTier](https://www.zerotier.com/).
  Use the host's VPN address.
- **Same version on both sides.** Both players should run the same build of
  the port.

The port adds two things on top of the game's original protocol to cope with
real internet links:

- **Pacing.** Each game packet is held until the game has consumed the
  previous one, so jitter can't overwrite state the game hasn't read yet.
  Tune this with `--hold N`.
- **Redundancy.** Every game packet is sent twice, to absorb loss. Disable
  this with `--no-redundancy`.

`--raw-net` turns both off. In testing, a game played smoothly with about
120 ms round trip and 5% packet loss. See [docs/network.md](docs/network.md)
for the details.

## Building

The build has two parts:

1. CMake runs the Python tools to extract the archive into `build/data/` and
   to generate C code into `build/gen/`.
2. CMake compiles that code together with the port.

Requirements:

- CMake 3.20 or newer
- a C11 compiler
- Python 3.8 or newer
- internet access on the first configure, to download SDL3 and stb_truetype

`-DBONKHEADS_SIT=/path/to/archive.sit` points the build at an archive
outside the repository. Both build scripts pass extra arguments on to CMake
and honour two environment variables:

- `BUILD_DIR` (default `build`) sets the output folder.
- `BUILD_TYPE` (default `RelWithDebInfo`) sets the build configuration.

### Windows

`build.bat` finds the newest Visual Studio that has the C++ tools, loads its
x64 environment, then configures and builds. Visual Studio ships CMake and
Ninja, so you only need to install Python separately. When you install
Python, tick **Add python.exe to PATH**.

To build by hand, run these commands from an "x64 Native Tools Command
Prompt":

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

### macOS

Install the Xcode command-line tools:

```bash
xcode-select --install
```

Then install CMake, Ninja (optional) and Python with
[Homebrew](https://brew.sh/):

```bash
brew install cmake ninja python
```

Build:

```bash
./build.sh
```

### Linux

Install a compiler, CMake, Python 3 and SDL3's build dependencies. On
Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake ninja-build python3 pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libdrm-dev libgbm-dev fonts-noto-core
```

```bash
./build.sh
```

On other distributions, see SDL's
[README-linux](https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md)
for the equivalent packages. Text uses Noto Sans or DejaVu Sans
(`fonts-noto-core` or `fonts-dejavu-core`), whichever is installed.

### Building without the game

`-DBONKHEADS_STUB_GAME=ON` builds the runtime, Toolbox and platform code
against an empty stub (`tools/stub/recomp_table.c`) instead of the
recompiled game. It needs neither the archive nor Python. The result only
runs `--help`, but it proves the port compiles and links; CI uses it.
`-DBONKHEADS_WERROR=ON` turns warnings into errors.

## Troubleshooting

| Problem | Fix |
|---|---|
| `Game archive not found` | Copy `Bonkheads_Deluxe.SIT` next to this README, or pass `-DBONKHEADS_SIT=...`. |
| `Python 3.8 or newer is required` | Install Python. On Windows, also tick "Add python.exe to PATH". Then delete `build/` and build again. |
| `no Visual Studio C++ compiler found` | Install the "Desktop development with C++" workload in the Visual Studio Installer. |
| First configure fails while downloading | Check your internet connection or proxy. SDL3 and stb_truetype are downloaded on the first build only. |
| Linux build fails inside SDL | A development package is missing; install the list above. |
| Joiner never shows up on the host | Check the port forward (UDP, not TCP) and the host's firewall, and that the joiner typed the host's *public* address. Try on one LAN first. |
| The game stops responding | The console shows a `watchdog:` line and a stack dump. Please include it in a bug report. |

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

The tools:

- **`tools/sit5.py`** extracts StuffIt 5 archives. It is a clean-room decoder
  for the "Arsenic" method (BWT, move-to-front and arithmetic coding).
- **`tools/m68k.py`** and **`tools/dumpcode.py`** form a 68000/68020
  disassembler. It uses the function names that CodeWarrior left in the
  binary (MacsBug symbols) and resolves the far-model jump table and
  relocations.
- **`tools/recomp.py`** is a static 68k-to-C recompiler:
  - Every original function becomes a C function with its original name
    (`f_Player1Processor`, `f_Boss1Brain`, …).
  - Liveness analysis skips condition codes that are never read.
  - Switch tables are resolved statically.

`src/toolbox` re-implements, in C, the roughly 300 Mac Toolbox and OS calls
the game uses. The game's own sprite blitters write straight into an
emulated 8-bit framebuffer. A separate "hardware" palette changes only
through `SetEntries`, as on real hardware, so the graphics and palette fades
come out pixel-exact. Text the game draws on screen is redrawn at full
window resolution in the system's UI font.

More detail:

- [docs/hle-spec.md](docs/hle-spec.md): what the game expects from the
  Toolbox
- [docs/network.md](docs/network.md): the network layer
- [docs/development.md](docs/development.md): working on the port, and the
  debugging and testing tools

## Status

**Verified on Windows 11:**

- intro, main menu, multi-colored high-score tables
- every menu button and dialog: Preferences and its sub-dialogs, Help,
  Info, Load Game and Quit
- one-player play and saved preferences
- a full two-player network game, including under simulated latency and loss

**Builds in CI** on Windows, Ubuntu and macOS. On macOS and Linux the game
itself has not been played yet.

**Not yet verified:**

- later worlds, bosses and the ending
- save/load and high-score entry over a full playthrough
- a real internet game across two NATs

**Known limitations:**

- Approximations:
  - QuickDraw regions are bounding rectangles.
  - Round rects and ovals draw as rectangles.
- Networking:
  - There is no NAT traversal or relay server, so the host must forward a
    port or both players must use a VPN.
  - A dropped connection isn't detected; restart both games.
- The level editor ("The Bonkreator") isn't ported.

## Legal

Bonkheads Deluxe is © 1997 1 A.M. Productions. This project is an
independent, unofficial port and includes none of the original game's code
or assets. You need your own copy of the game. The third-party libraries
downloaded at build time keep their own licenses:

- SDL3: zlib license
- stb_truetype: public domain / MIT
