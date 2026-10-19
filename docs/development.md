# Working on the port

This page covers the layout of the source, the build pipeline, and the tools
for debugging and testing. For what the game expects from the Mac Toolbox,
see [hle-spec.md](hle-spec.md). For the network layer, see
[network.md](network.md).

## Ground rule: nothing from the game goes into git

The original game is copyrighted. The repository must contain only the
port's own code, tools and docs. Never commit any of these:

- the `.SIT` archive
- extracted resources
- generated C
- disassembly listings
- screenshots or logs that show game assets

`.gitignore` excludes `*.SIT`, `build/`, `extracted/`, `disasm/` and `out/`.
Keep generated output in those folders.

## Source layout

| Path | Contents |
|---|---|
| `tools/sit5.py` | StuffIt 5 parser and Arsenic decoder (BWT, MTF, adaptive arithmetic coding, randomization, RLE). |
| `tools/rsrc.py` | Resource fork reader. |
| `tools/m68k.py` | 68000/68020 disassembler. It records operand details for the recompiler. |
| `tools/a5world.py` | Rebuilds the initial A5 world: DATA 0 decompression, the relocation lists and the jump table. |
| `tools/dumpcode.py` | Writes per-segment listings, split on MacsBug symbols. Useful for reading the game. |
| `tools/recomp.py` | The 68k-to-C recompiler. |
| `tools/traps.py` | Trap number to name table. |
| `tools/readprefs.py`, `tools/bmp2png.py`, `tools/shot.ps1` | Debug helpers (see below). |
| `tools/stub/` | Empty game image for engine-only builds (`-DBONKHEADS_STUB_GAME=ON`). |
| `tools/tests/` | Unit tests for the tools. They need no game data. |
| `src/runtime/` | CPU state, condition-code helpers, memory access and the dispatch between recompiled functions. `layout.h` holds the memory map shared with `recomp.py`. |
| `src/toolbox/` | The Mac Toolbox reimplementation, one file per manager. The main files are `qd.c` (QuickDraw), `qdcolor.c` (GDevices, GWorlds, Palette Manager), `text.c`, `window.c`, `dialog.c`, `event.c`, `res.c`, `file.c`, `mem.c`, `sound.c`, `misc.c` (Gestalt, packages, Standard File) and `net.c`. |
| `src/toolbox/patches.c` | Port-side adjustments to game data (the optional `highscores.cfg`). |
| `src/toolbox/overlay.c` | The port's own on-screen banner and hover hints. |
| `src/platform/` | SDL3 window, input, audio and timing; UDP sockets; logging; crash and hang stack dumps. |
| `src/main.c` | Command-line parsing and startup. |

## Build pipeline

`CMakeLists.txt` drives everything:

1. **Extract.** `sit5.py --extract` unpacks the archive into
   `build/data/Bonkheads Deluxe/`, writing the `.rsrc` and `.finf` sidecars.
2. **Recompile.** `recomp.py` reads the CODE and DATA resources from
   `Bonkheads Deluxe.rsrc` and writes `build/gen/recomp_seg*.c`,
   `recomp_table.c` and `recomp_funcs.h`.
3. **Compile.** The generated C is compiled with warnings off, the port
   sources with warnings on, and both are linked statically against SDL3.

Extraction reruns when the archive or `sit5.py` changes. The recompile step
reruns when any `tools/*.py` changes.

## How the recompiler works

The game was built with CodeWarrior's 68k far model. Its parts:

- **CODE 0** holds the jump-table header.
- **CODE 1** holds the startup runtime.
- **Every other segment** starts with a 12-byte header: the jump-table
  offset, the entry count and the relocation offset.
- **DATA 0** holds the initial globals. They are RLE-compressed and come
  with 6 relocation lists.
- **Jump-table slots** have the form `[A9F0/4EF9][offset][segment]`.

`recomp.py` works in four steps:

1. **Discover the code.** It follows control flow by recursive descent from
   every jump-table entry and every MacsBug-named function. Any call target
   it finds becomes a function entry. This repeats until nothing new
   appears, which also finds calls into runtime helpers.
2. **Emit one C function per original function.** Each instruction becomes
   C that operates on the emulated registers (`D[]`, `A[]`) and on
   big-endian memory accessors. Branches become `goto`s.
3. **Analyse flag liveness.** Condition codes are computed only where a
   later instruction reads them. This is the main speed win.
4. **Resolve switch tables statically.** The bounds come from the compare
   in the fall-through predecessor.

At run time:

- Calls through the jump table, `JSR (An)` and similar go through
  `call_addr`, which maps a 68k address to its C function.
- Traps go through `trap.c`.
- `call_emulated` lets the Toolbox call back into game code, for example
  completion routines, `ProcessDDPPacket` and dialog filters.
- `g_cur_fn` and `g_cur_trap` track the current function and trap, for
  diagnostics.

When the recompiler meets something it can't translate, it emits a call
that logs the address and stops. The log then names the function to look
at.

## Adding or fixing a Toolbox call

1. Run with `--trace` to see the calls the game makes, and `dumpcode.py` to
   read the code around the call.
2. Implement or fix the trap in the matching `src/toolbox/*.c` file. Use
   the `ARG16`/`ARG32` and `RET16`/`RET32` helpers for Pascal-convention
   Toolbox traps. OS traps take their arguments in registers and must set
   D0, from which the dispatcher sets the condition codes.
3. Write down any non-obvious game dependency in `docs/hle-spec.md`.

## Debugging aids

| Flag | Effect |
|---|---|
| `--trace` | Log every Toolbox call. |
| `--netlog` | Log network events. |
| `--headless` | Hidden window; real keyboard and mouse input is ignored. |
| `--shot SECONDS FILE.bmp` | Save a screenshot of the final output, overlays included, and exit. Also logs any `--dump-a5` regions. |
| `--press SECONDS:MACKEY[:FRAMES]` | Scripted key press. Takes a Mac virtual key code, e.g. `0x24` Return, `0x31` Space, `0x7B`/`0x7C`/`0x7E` arrows. |
| `--click SECONDS:X:Y` / `--move SECONDS:X:Y` | Scripted mouse click or move, in game coordinates (640×480). |
| `--dump-a5 OFFSET:LEN` | Log game globals at A5+OFFSET at `--shot` time. Repeatable; offsets are usually negative. |
| `--pixel-text` | Use the original bitmap text, to compare with the high-resolution text layer. |

**Hangs.** A watchdog thread notices when the game stops reaching the event
loop for 3 seconds. It then logs the current recompiled function and trap,
and on Windows a native stack trace of the main thread. The default
`RelWithDebInfo` build keeps the symbols for this.

**Settings.** `tools/readprefs.py "build/data/Bonkheads Deluxe/Bonkheads
Deluxe Prefs.rsrc"` prints the stored keys, volume, music, difficulty and
level set.

## Automated testing without disturbing a running game

`tools/shot.ps1` runs the game headless and saves a screenshot (Windows):

```bash
powershell -File tools/shot.ps1 20 menu --press 3:0x31 --press 6:0x31
```

That example produces `build/logs/menu.png` and `build/logs/menu.log`. Each
run gets a private copy of the game data in `build/testdata/<name>/`,
without your preferences or `network.cfg`. Tests therefore never touch your
own settings, saves or high scores, and the hidden window never takes focus.

Environment variables:

- `BONK_TEST_NETCFG`: seeds the copy's `network.cfg`, for example
  `host_address=127.0.0.1:24732`.
- `BONK_SOUND=1`: keeps audio on.
- `BONK_SHOT_DIR`: puts the PNG somewhere else.

On macOS and Linux, run the same thing by hand:

```bash
mkdir -p /tmp/bonk-test && cp "build/data/Bonkheads Deluxe/"* /tmp/bonk-test/ && rm -f /tmp/bonk-test/"Bonkheads Deluxe Prefs"*
```

```bash
./build/bonkheads --headless --nosound --data /tmp/bonk-test --shot 20 /tmp/menu.bmp --press 3:0x31
```

**Network tests on one machine.** Start a host with `--port 24732` and a
joiner with `--connect 127.0.0.1:24732`. Add `--sim-latency` and
`--sim-loss` to either side.

## Unit tests and CI

These tests check the disassembler and resource reader on synthetic input:

```bash
python -I -m unittest discover -s tools/tests -v
```

GitHub Actions (`.github/workflows/ci.yml`) runs two jobs:

- builds the engine against the stub game on Windows, Ubuntu and macOS with
  warnings as errors, then runs `--help`
- runs the tool tests

CI never sees the game. Changes to the recompiler must be checked locally
against your own copy.
