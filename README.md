# Bonkheads Deluxe Port

An unofficial port of *Bonkheads Deluxe* (1 A.M. Productions, 1997) from the
classic 68k Macintosh to Windows, macOS and Linux. The game's original
AppleTalk multiplayer mode is carried over UDP and works over the internet.

This repository does not contain the game. A copy of the original
`Bonkheads_Deluxe.SIT` archive is required. At build time the archive is
extracted and the game's 68k code is statically recompiled to C. All output
derived from the game stays in the local `build/` directory. The project
cannot help with obtaining the game.

## Contents

- [Requirements](#requirements)
- [Getting started](#getting-started)
- [Controls](#controls)
- [Command-line options](#command-line-options)
- [Network play](#network-play)
- [Data files](#data-files)
- [Troubleshooting](#troubleshooting)
- [Building manually](#building-manually)
- [Architecture](#architecture)
- [Project status](#project-status)
- [Credits and license](#credits-and-license)

## Requirements

All platforms need CMake 3.20 or later, a C11 compiler, Python 3.8 or
later, and internet access for the first build, which downloads SDL3 and
stb_truetype.

### Windows

- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) or
  later (the Community edition is sufficient), with the *Desktop development
  with C++* workload. It includes CMake and Ninja.
- [Python 3](https://www.python.org/downloads/), installed with *Add
  python.exe to PATH* selected.

### macOS

```bash
xcode-select --install
```

```bash
brew install cmake ninja python
```

### Linux

The following installs the dependencies on Debian and Ubuntu:

```bash
sudo apt-get install build-essential cmake ninja-build python3 pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libdrm-dev libgbm-dev fonts-noto-core
```

For other distributions, see SDL's
[README-linux](https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md).

## Getting started

1. Place `Bonkheads_Deluxe.SIT` in the repository root. The build also
   looks in the user's Downloads and Desktop folders.
2. Run the launcher script. It builds the game if needed and then starts it.
   - Windows: run `play.bat`, or double-click it.
   - macOS and Linux:

     ```bash
     ./play.sh
     ```

The first build takes a few minutes. Later runs only rebuild what has
changed.

## Controls

| Player | Left | Jump | Right |
|---|---|---|---|
| Player 1 | Left arrow | Up arrow | Right arrow |
| Player 2 | Ctrl | Windows / Super (Option on macOS) | Alt (Command on macOS) |

The game refers to the Macintosh modifier keys. On Windows and Linux they
are mapped by physical position:

| In the game | macOS | Windows / Linux |
|---|---|---|
| CNTL | Control | Ctrl |
| OPTN | Option | Windows / Super |
| CMND, ⌘ | Command | Alt |

Other controls:

| Action | Input |
|---|---|
| Pause | Caps Lock |
| Toggle fullscreen | Alt+Enter |
| Quit | QUIT on the main menu, or ⌘Q (Alt+Q on Windows and Linux) |
| Rebind keys | Preferences → Player Controls |

Gamepads are supported. The first connected gamepad controls player 1 and
the second controls player 2. The d-pad or left stick moves and the bottom
face button (A) jumps. Gamepad input follows each player's key bindings.
Menus require a mouse and keyboard.

During gameplay the window grabs the keyboard so that the Windows/Super key
does not open the system menu; this also disables Alt+Tab until a menu is
shown. The `--no-keyboard-grab` option disables this behaviour.

## Command-line options

Options are passed to the game executable, or through the launcher scripts,
for example `play.bat --fullscreen`.

| Option | Description |
|---|---|
| `--fullscreen` | Start in fullscreen mode. |
| `--scale N` | Initial window scale factor (default 2). The window is resizable. |
| `--nosound` | Disable audio. |
| `--pixel-text` | Render text with the original bitmap fonts. |
| `--no-keyboard-grab` | Do not grab the keyboard during gameplay. |
| `--data DIR` | Load game data from `DIR`. |
| `--port N` | UDP port used when hosting (default 24731). |
| `--connect HOST[:PORT]` | Host address for NET JOIN; skips the address prompt. |

`--help` lists all options, including the network tuning and debugging
options described in [docs/network.md](docs/network.md) and
[docs/development.md](docs/development.md).

## Network play

Network games have two players: a host and a joiner. Only the host's port
must be reachable.

**Host**

1. Select NET HOST. The game displays the computer's local address and the
   port in use.
2. Give the joiner the local address (same network) or the public IP
   address (internet). For internet play, forward UDP port 24731 on the
   router to the host computer and allow the game through the firewall.
3. When the joiner's name appears in the list, select it and click OK.

**Joiner**

1. Select NET JOIN.
2. Enter the host's address, for example `203.0.113.7`, or
   `203.0.113.7:5000` for a non-default port. The address is saved for the
   next session. On a local network, the field can be left empty to search
   for a host automatically.

If port forwarding is not possible, a virtual LAN such as
[Tailscale](https://tailscale.com/) or [ZeroTier](https://www.zerotier.com/)
can be used instead, with the host's virtual LAN address.

Both players should run the same version of the port. To tolerate internet
latency and packet loss, the transport layer paces packet delivery and
sends each game packet twice. Tests with 120 ms round-trip time and 5%
packet loss were playable. See [docs/network.md](docs/network.md) for the
protocol details.

## Data files

The game stores its files in the data directory,
`build/data/Bonkheads Deluxe/`, as the original stored them alongside the
application. Rebuilding does not modify these files.

| File | Contents |
|---|---|
| `Bonkheads Deluxe Prefs.rsrc`, `Bonkheads Deluxe Prefs.finf` | Settings and high scores. Delete both files to reset them. |
| Saved games | One file set per save, named in the save dialog. |
| `network.cfg` | The last address entered in NET JOIN. |
| `highscores.cfg` | Optional additional entries for the default high-score table. |

Each Macintosh file is stored as up to three host files: `NAME` (data
fork), `NAME.rsrc` (resource fork) and `NAME.finf` (file type and creator).
Copy all three when moving a saved game.

`highscores.cfg` contains one `name, score, level` entry per line. The
entries are merged into the high-score table only when the table is newly
created:

```
Alice, 13337, 10
Bob, 5000, 4
```

## Troubleshooting

| Problem | Resolution |
|---|---|
| `Game archive not found` | Place `Bonkheads_Deluxe.SIT` in the repository root, or pass `-DBONKHEADS_SIT=PATH`. |
| `Python 3.8 or newer is required` | Install Python (on Windows, with *Add python.exe to PATH*), delete `build/`, and build again. |
| `no Visual Studio C++ compiler found` | In the Visual Studio Installer, select *Modify* and add the *Desktop development with C++* workload. |
| The first build fails while downloading | Check the network connection or proxy. SDL3 and stb_truetype are downloaded on the first build only. |
| The Linux build fails in SDL | A development package is missing. Install the packages listed under [Requirements](#linux). |
| The host does not see the joiner | Verify that the forwarded port is UDP, that the host's firewall allows the game, and that the joiner used the host's public address. Test on a local network first. |
| The game stops responding | The console log contains a `watchdog:` entry with diagnostic information. Include it in bug reports. |

## Building manually

The launcher scripts call `build.bat` (Windows) and `build.sh` (macOS,
Linux), which can also be run directly. `build.bat` locates Visual Studio
automatically. The equivalent CMake commands are shown below; on Windows,
run them from an *x64 Native Tools Command Prompt*.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

| Setting | Description |
|---|---|
| `-DBONKHEADS_SIT=PATH` | Path to the game archive. The build scripts pass extra arguments to CMake. |
| `BUILD_DIR` | Environment variable for the build scripts: output directory (default `build`). |
| `BUILD_TYPE` | Environment variable for the build scripts: build configuration (default `RelWithDebInfo`). |
| `-DBONKHEADS_STUB_GAME=ON` | Build the engine against an empty stub instead of the game. Requires neither the archive nor Python; used by CI. |
| `-DBONKHEADS_WERROR=ON` | Treat compiler warnings as errors. |

## Architecture

```
Bonkheads_Deluxe.SIT ──tools/sit5.py──▶ Mac files and resources (code, pictures, sounds, levels)
        CODE resources ──tools/recomp.py──▶ build/gen/*.c   (one C function per original function)
                                             │
            src/runtime  68k registers, big-endian memory, calls between functions
            src/toolbox  the Mac Toolbox in C: QuickDraw, windows, dialogs, events,
                         resources, files, sound, AppleTalk over UDP
            src/platform SDL3 window, input, audio and UDP sockets
```

The game's 68k code is translated ahead of time into C. Each original
function becomes a C function with its original symbol name, for example
`f_Player1Processor`. The approximately 300 Macintosh Toolbox and OS calls
the game uses are reimplemented in C on top of SDL3. Graphics are rendered
into an emulated 8-bit framebuffer and are pixel-exact. On-screen text is
re-rendered at display resolution using the system UI font.

Further documentation:

- [docs/development.md](docs/development.md): source layout, recompiler,
  debugging and testing
- [docs/hle-spec.md](docs/hle-spec.md): the game's requirements of the
  Toolbox
- [docs/network.md](docs/network.md): network transport and protocol

## Project status

Tested on Windows 11:

- intro, main menu and high-score tables
- all menus and dialogs
- all twelve worlds, their bosses and the finale levels
- local two-player games and the Bonus Levels set
- saving and loading one- and two-player games
- game over and high-score entry
- persistence and application of settings
- two-player network games, including under simulated latency and packet
  loss

CI builds the engine on Windows, Ubuntu and macOS.

Not yet tested:

- gameplay on macOS and Linux
- the ending, reached without debugging shortcuts
- network games between two separate networks over the internet

Known limitations:

- There is no NAT traversal or relay server; the host must accept incoming
  UDP traffic.
- Disconnections are not detected during network games.
- The level editor (*The Bonkreator*) is not ported.
- Ovals and rounded rectangles are drawn as rectangles.

## Credits and license

*Bonkheads Deluxe* was developed by 1 A.M. Productions (© 1997) and
published by Aspyr Media. The game, including its characters, artwork,
sound, music and levels, is their work, and all credit for it belongs to
them.

This is an independent, unofficial project. It contains no code or assets
from the original game. The source code of the port is dedicated to the
public domain under the [Unlicense](LICENSE); its author claims no rights
to it.

The build downloads the following libraries, which are distributed under
their own licenses:

- [SDL3](https://www.libsdl.org/) (zlib License)
- [stb_truetype](https://github.com/nothings/stb) (public domain or MIT)
