# HLE notes: how the game uses the Mac Toolbox

These notes summarise what static analysis of the 68k code, and getting
the game to run, showed about the Toolbox. They record what the
reimplementation in `src/toolbox` must do, including the places where the
game depends on subtle behaviour of the real Mac. Code locations are given as
seg:offset, and globals as offsets from A5.

The recompiled game sees a flat, big-endian 64 MB memory. The layout is
shared by `tools/recomp.py` and `src/runtime/layout.h`:

| Region | Address |
|---|---|
| CODE segments | from $10000 |
| A5 world (application globals below A5, jump table above) | A5 = $88000 |
| Stack (grows down) | top at $200000 |
| Handle heap | from $200000 |

Toolbox traps (`$Axxx` words) are dispatched to C functions in `trap.c`. They
use the Pascal calling convention: arguments are pushed left to right, and
the callee pops them and leaves the result on the stack. OS traps take their
arguments in registers and set the condition codes from D0, which the game
tests.

## Trap name corrections

| Trap | Name |
|---|---|
| AA18 | GetCTable |
| AA24 | DisposeCTable |
| AA29 | GetDeviceList |
| AA2A | GetMainDevice |
| AA2C | TestDeviceAttribute |
| AA31 | SetGDevice |
| AA32 | GetGDevice |
| AA39 | MakeITable |
| AA3F | SetEntries |
| AA45 | NewCWindow |
| AAF2 | ControlStripDispatch (return an error) |
| A851 | SetCursor |
| A852 | HideCursor |
| A853 | ShowCursor |
| A746 / A346 / A647 | Get/SetToolTrapAddress |
| A198 | HWPriv |
| A1AD | Gestalt |
| A12F | PPostEvent |

Gestalt is used only through the seg7 glue. That glue relies on
`GetToolTrapAddress($A89F) != GetOSTrapAddress($A0AD)`. Two selectors matter:

- `'addr'` must return 7.
- `'sdev'` should return an error.

## Startup

1. StandardInits
2. In32BitAddressingMode
3. HideControlStrip
4. InstallMenus (129, 130), HideMenuBar
5. CheckMonitor (needs pixelSize 8)
6. MakePalettes: opens "BDX_Data" and calls GetCTable 128/130/200/201, then NewPalette and DetachResource
7. NewCWindow 640x480, plainDBox
8. Interface: the main menu
9. Execution: the game

## Graphics

- **Screen.** It is exactly 640x480 at 8 bpp. The main GDevice provides
  gdPMap (+22), gdNextGD (+30) and gdRect (+34). The PixMap's pmTable is
  at +42.
- **Window ports.** A window's portPixMap must alias the screen framebuffer.
  GetPixBaseAddr on it returns the screen base, and the game writes
  directly to it.
- **GWorlds (`$AB1D`).** Selectors used:

  | Selector | Call |
  |---|---|
  | $160000 | NewGWorld(&gw, 8, &r, ctab, nil, 0) |
  | $40001 | LockPixels |
  | $40002 | UnlockPixels |
  | $40004 | DisposeGWorld |
  | $40007 | CTabChanged |
  | $4000F | GetPixBaseAddr |
  | $40017 | GetGWorldPixMap |

  Set rowBytes to 640 with the $8000 flag set. Pixels never move.
- **CopyBits.** The game mostly uses srcCopy with a nil mask (srcOr, srcXor
  and srcBic are supported as well). Clip to the destination bounds, the port clip and
  the mask region. Between 8-bit pixmaps, copy indices raw when the two
  color tables are the same table or have the same `ctSeed`. Otherwise
  translate each index through RGB to the nearest destination color, as
  Color QuickDraw does. The Help screens rely on this translation.
- **The ctSeed rule.** Color QuickDraw treats two color tables with equal
  `ctSeed` as identical. The game stamps the screen's seed into its GWorld
  tables, so drawing into those GWorlds must match colors against the
  *screen's* table, not the GWorld's own copy (`qd_effective_ctab`).
- **Colors in ports.** `RGBForeColor` and `RGBBackColor` pick the nearest
  palette entry. `ForeColor` and `BackColor` take the classic 8 colors,
  which map from their CMYK bits. The game also writes palette *indices*
  straight into `fgColor`/`bkColor` of color ports, for example the
  high-score colors. Honour these as indices.
- **Window occlusion.** Drawing into a window must not touch the parts of
  the screen covered by windows in front of it. The main menu keeps
  redrawing its backdrop while dialogs are open.
- **Sprt resource.** A Rect at +0. From +8, a stream of 32-bit opcode words
  (`op = w>>24`, `n = w & 0xFFFFFF`):

  | op | Meaning |
  |---|---|
  | 0 | end |
  | 1 | new row (n = byte length of that row) |
  | 2 | n literal pixels, padded to 4 bytes |
  | 3 | skip n pixels |

  The game renders these itself.
- **DrawPicture.** Pictures are PICT v2. They use $98 PackBitsRect (8-bit
  with a clut) and two use $9A DirectBits (32-bit). Map colors to the
  destination palette by nearest RGB.
- **Palettes.** Fades modify the device ctab and call SetEntries(0, 255,
  table) once per step. The hardware palette must be separate and change
  only through SetEntries. SetEntries waits about 1 tick, which paces the
  fades.
- **Palette Manager.** Some screens, such as Help, attach their own
  palette with SetPalette/ActivatePalette. When the window is (or comes to
  the) front, the palette's colors are loaded into the device table and,
  unless the screen is faded to black, into the hardware palette. While the
  screen is black, the game's next fade-in reveals the new colors.
- **Fonts.**

  | Font | Where used |
  |---|---|
  | Thwack (FOND/sfnt 1470) | Fetched with GetFNum. A RealFont check at size 12 falls back to Geneva (3) |
  | Helvetica (21) | |
  | Geneva (3) | |
  | System (0) | |

  Text calls used: DrawString, StringWidth, CharWidth and TETextBox (center).

  The port renders text drawn to the screen a second time, at output
  resolution, in the OS UI font (`text.c`). These high-resolution entries
  are composited over the 8-bit framebuffer and dropped when anything paints
  over them. Window and popup save-under saves and restores them with the
  pixels. Text drawn into offscreen buffers is anti-aliased into the
  palette.

## Main menu

The main menu is a custom polled loop, not an event loop. It uses GetMouse,
Button, GetKeys and FlushEvents. The hotspots are drawn from PICTs
10500, 9500 and 9501.

Dialogs (DLOG resources) are needed for:

- 131: prefs. Includes a popup user item, checkboxes, radios and a filter.
- 129: keys
- 134: 2-player load
- 133: level jump
- 750: confirm
- 506: high-score name
- 10700: save
- 199: error log
- 10001, 10002, 10004: network

Alerts used: 300, 350, 10000, 10001, 10005, 20000–20003.

## Files

The port presents the game data folder as a single HFS volume (`file.c`).
A Mac file `NAME` is stored as `NAME` (data fork), `NAME.rsrc` (resource
fork) and `NAME.finf` (Finder type and creator). All files are opened by
name relative to the app folder:

- "BDX_Data", opened and closed repeatedly
- the level set named in the prefs (default "Original Levels")

The prefs file is "Bonkheads Deluxe Prefs" in FindFolder('pref'), which
the port maps to the game data folder. It holds:

- 'Bonk' 0: 0x914 bytes of settings
- 'HiSc' 128/129: the one- and two-player high scores. Clearing them
  calls AddResource with a handle that is already a resource. That must
  fail with `addResFailed`; it must not free the handle.

Byte offsets in 'Bonk' 0 that matter (`tools/debug/readprefs.py` prints them):

| Offset | Setting |
|---|---|
| 0, 2, 4 | Player 1 keys: left, jump, right |
| 6, 8, 10 | Player 2 keys: left, jump, right |
| 1548 | volume popup item (11 = off) |
| 1550 | music on/off |
| 1552 | difficulty (0 easy, 1 medium, 2 hard) |
| 2067 | level set name (Str63) |

At runtime the game copies these into globals: difficulty to A5-32652,
music to A5-3920 and the key codes from A5-32640.

Saved games are written with StandardPutFile into a file of type 'BKSV'
holding an 'SVGM' 128 resource. They are loaded with StandardGetFile.

## Memory

- MaxMem/MaxBlock must return at least 300000.
- Handles must never move once allocated.
- Detaching or disposing the same handle twice must be tolerated. So must
  DisposePtr on a pointer the heap does not own.

## Dialogs

- ModalDialog must deliver exactly one update event to a new dialog.
  Delivering it on every pass makes the Preferences filter redraw forever.
- HideControl erases the control's rectangle.

## Sound

- The game uses 48 `snd ` resources. Most are 8-bit unsigned mono. Two use
  the extended 16-bit header.
- Each resource holds one bufferCmd ($8051).
- There are 6 channels plus a "start" channel.
- PlaySound sends flushCmd and quietCmd, then calls SndPlay asynchronously.
- MyInstallCallback queues callBackCmd(13, param1=1, param2=A5). The callback
  sets an "idle" flag.
- flushCmd must drop a pending callback without running it.
- Music queues 120 bufferCmds in a single call, so the queue must hold at
  least 128 commands.
- SndPlay with async=0 blocks until the sound finishes.
- SoundDispatch $022C0018 is GetDefaultOutputVolume and $02300018 is
  SetDefaultOutputVolume.

## Input and timing

- GetKeys uses the standard KeyMap bit order. Caps Lock ($39) is reported as
  its latched state; it toggles pause. The low-memory KeyMap ($174) must
  mirror the same state.
- Default controls:
  - Player 1: arrow keys. Left $7B, up $7E, right $7C.
  - Player 2: Control $3B (left), Option $3A (jump), Command $37 (right).
- The bindings live in word globals, key code in the low byte: player 1
  left/right/jump at A5-32640/-32642/-32644, player 2 at
  -32646/-32648/-32650. SetGlobals fills them from the prefs when a game
  starts; StartWacky swaps left and right. The platform layer reads them
  every frame to turn gamepad input into KeyMap bits.
- Command-Q quits.
- MainLoop waits until `TickCount >= t0 + 2`, so the game runs at 30 fps.
- Random is the Toolbox Random on the QD randSeed at A5+$67D4. The seed is
  set from low-memory Time ($20C).

## Network

See [network.md](network.md).
