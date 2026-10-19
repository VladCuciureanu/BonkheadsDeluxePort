# Bonkheads Deluxe for Windows, macOS and Linux

A port of **Bonkheads Deluxe** (1 A.M. Productions, 1997), a two-player
platform game for the classic Mac, to Windows, macOS and Linux. The original
AppleTalk network mode works over the internet.

You need your own copy of the game, `Bonkheads_Deluxe.SIT`. This repository
contains none of the game's code, graphics, sound or levels. When you build,
your archive is unpacked and the game's 68k machine code is recompiled to C.
Everything produced from the game stays in the local `build/` folder.

## Quick start

**1. Install the tools.** This is needed once per computer.

<details>
<summary><b>Windows</b></summary>

- [Visual Studio 2022 or newer](https://visualstudio.microsoft.com/downloads/).
  The free Community edition is enough. In the installer, tick the
  **Desktop development with C++** workload.
- [Python 3](https://www.python.org/downloads/). In the installer, tick
  **Add python.exe to PATH**.

</details>

<details>
<summary><b>macOS</b></summary>

```bash
xcode-select --install
```

```bash
brew install cmake ninja python
```

The second command needs [Homebrew](https://brew.sh/).

</details>

<details>
<summary><b>Linux (Debian/Ubuntu)</b></summary>

```bash
sudo apt-get install build-essential cmake ninja-build python3 pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libdrm-dev libgbm-dev fonts-noto-core
```

For other distributions, SDL's
[README-linux](https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md)
lists the equivalent packages.

</details>

**2. Put `Bonkheads_Deluxe.SIT` in this folder.** Your Downloads folder or
your Desktop also work.

**3. Start the game.**

- **Windows:** double-click `play.bat`.
- **macOS and Linux:** run this:

  ```bash
  ./play.sh
  ```

The first run downloads SDL3 and builds the game, which takes a few
minutes. After that the game starts in seconds. Use the same command every
time you want to play; it only rebuilds what has changed.

## Controls

| Player | Left | Jump | Right |
|---|---|---|---|
| Player 1 | ← | ↑ | → |
| Player 2 | Control | Option (Alt) | Command (Windows / Super key) |

- **Change the keys** in **Preferences → Player Controls**.
- **Pause** with Caps Lock, as on the original Mac.
- **Fullscreen** toggles with Alt+Enter.
- **Menu buttons** show what they do when you hover over them.
- **Quit** with **QUIT** on the main menu.

Game options go after the command, for example `play.bat --fullscreen` or
`./play.sh --scale 3`:

| Option | Effect |
|---|---|
| `--fullscreen` | Start in fullscreen. |
| `--scale N` | Window size multiplier (default 2). You can also drag the window to any size. |
| `--nosound` | Turn audio off. |
| `--pixel-text` | Use the original low-resolution fonts. |

`--help` lists every option.

## Playing over the internet

One player hosts and the other joins. Only the host needs to set up their
router.

**The host clicks NET HOST.**

1. A banner shows your address. Send it to your friend:
   - **On the same Wi-Fi or network:** send the local address shown, for
     example `192.168.1.20`.
   - **Over the internet:** send your public IP address (search for "what is
     my IP"). Then, in your router, forward **UDP port 24731** to this
     computer. If your firewall asks, allow the game.
2. When your friend's name appears in the list, select it and click **OK**.

**The joiner clicks NET JOIN.**

1. Type the address the host sent, for example `203.0.113.7`. It's
   remembered for next time.
2. If you're on the same network, you can leave the box empty, and the game
   finds the host by itself.

**If you can't forward a port**, install a free VPN such as
[Tailscale](https://tailscale.com/) or [ZeroTier](https://www.zerotier.com/)
on both computers. Then use the host's VPN address instead.

**Other details:**

- Both players should use the same version of this port.
- To use another port, the host starts the game with `--port N`, and the
  joiner types `ADDRESS:N`.
- The port smooths over lag and lost packets. A game played well at about
  120 ms round trip with 5% packet loss.
- [docs/network.md](docs/network.md) explains how it works.

## Settings, saves and high scores

Everything the game writes goes into the game data folder,
`build/data/Bonkheads Deluxe/`, as the original kept its files next to the
application. Rebuilding never touches these files.

| File | Contents |
|---|---|
| `Bonkheads Deluxe Prefs.rsrc` and `.finf` | Settings and high scores. Delete both to reset. |
| Saved games | One file per save, with the name you gave it. |
| `network.cfg` | The address last typed into NET JOIN. |
| `highscores.cfg` | Optional. Your own entries for the default high-score table. |

Each Mac file is stored as up to three files. `NAME` holds the data,
`NAME.rsrc` the resources and `NAME.finf` the Mac file type. Keep the three
together when you copy a saved game.

To put your own names in the default high-score table, create
`highscores.cfg` with one `name, score, level` entry per line:

```
Alice, 13337, 10
Bob, 5000, 4
```

The entries only go into a fresh table. Delete the prefs files to see them.

## Troubleshooting

| Problem | What to do |
|---|---|
| `Game archive not found` | Put `Bonkheads_Deluxe.SIT` in this folder, then run again. |
| `Python 3.8 or newer is required` | Install Python. On Windows, tick "Add python.exe to PATH". Then delete the `build` folder and run again. |
| `no Visual Studio C++ compiler found` | Open the Visual Studio Installer, choose **Modify**, and tick **Desktop development with C++**. |
| The first build fails while downloading | Check your internet connection. The first build downloads SDL3 and stb_truetype. |
| The Linux build fails inside SDL | A development package is missing; install the list in Quick start. |
| The host never sees the joiner | Check that the forwarded port is **UDP** (not TCP), that the host's firewall allows the game, and that the joiner typed the host's *public* address. Try on one network first. |
| The game freezes | The console window shows a `watchdog:` message. Please include it when you report the problem. |

## Building by hand

`play.bat` and `play.sh` call `build.bat` and `build.sh`, which you can
also run on their own. On Windows, `build.bat` finds Visual Studio itself.
Both scripts run two CMake commands, which you can also type yourself, from
an "x64 Native Tools Command Prompt" on Windows:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

The build needs CMake 3.20 or newer, a C11 compiler and Python 3.8 or newer.
Options:

| Setting | Effect |
|---|---|
| `-DBONKHEADS_SIT=PATH` | Use an archive somewhere else. Extra arguments to the build scripts go to CMake. |
| `BUILD_DIR`, `BUILD_TYPE` | Environment variables for the scripts: output folder (default `build`) and configuration (default `RelWithDebInfo`). |
| `-DBONKHEADS_STUB_GAME=ON` | Build the engine without the game, for CI. It needs neither the archive nor Python, and only runs `--help`. |
| `-DBONKHEADS_WERROR=ON` | Treat warnings as errors. |

## How it works

```
Bonkheads_Deluxe.SIT ──tools/sit5.py──▶ Mac files and resources (code, pictures, sounds, levels)
        CODE resources ──tools/recomp.py──▶ build/gen/*.c   (one C function per original function)
                                             │
            src/runtime  68k registers, big-endian memory, calls between functions
            src/toolbox  the Mac Toolbox rewritten in C: QuickDraw, windows, dialogs,
                         events, resources, files, sound, AppleTalk → UDP
            src/platform SDL3 window, input, audio and UDP sockets
```

The game's original machine code is translated once, ahead of time, into C
that keeps the original function names, such as `f_Player1Processor` and
`f_Boss1Brain`. About 300 Mac system calls the game uses are reimplemented
on top of SDL3. The graphics are pixel-exact, and on-screen text is redrawn
sharply in your system's font.

More about the internals:

- [docs/development.md](docs/development.md): the source layout, the
  recompiler, and the debugging and testing tools
- [docs/hle-spec.md](docs/hle-spec.md): what the game expects from the Mac
  Toolbox
- [docs/network.md](docs/network.md): the network layer

## Status

**Works (tested on Windows 11):**

- the intro, the main menu and the high scores
- every menu and dialog
- one-player play
- settings that are saved and applied
- two-player network games, including over slow and lossy links

The engine also builds on macOS and Linux in CI.

**Not yet tested:**

- playing on macOS and Linux
- a full playthrough, including later worlds, bosses and the ending
- an internet game between two homes

**Known limitations:**

- Networking:
  - The host must forward a port, or both players must use a VPN.
  - A dropped connection isn't detected; restart both games.
- The level editor ("The Bonkreator") isn't ported.
- A few shapes are approximated: round rectangles and ovals draw square.

## Legal

Bonkheads Deluxe is © 1997 1 A.M. Productions. This is an independent,
unofficial port that contains none of the original game's code or assets,
so you need your own copy of the game.

The build downloads these libraries, which keep their own licenses:

- [SDL3](https://www.libsdl.org/) (zlib)
- [stb_truetype](https://github.com/nothings/stb) (public domain or MIT)
