## m8emu for Windows 0.1.1

### Fixed

- Flicker during playback: vsync is now on by default on Windows. With it
  off, Direct3D presented frames mid-refresh (tearing), which screen
  recordings do not show. `m8emu.log` records the renderer and vsync state
  (`video renderer=... vsync=1`); `--vsync 0` turns it off again.

## m8emu for Windows 0.1.0

A Windows build of the m8emu Dirtywave M8 Headless emulator, as one
executable (`m8emu-0.1.1-windows-x64.exe`). Windows 10 (1903) or newer, x64.

### First run

The M8 Headless firmware is **not included**. Get it free from Dirtywave
(the M8 Headless firmware page), then start `m8emu.exe` and either click
**CHOOSE FILE...** and pick `M8_V6_5_2C_HEADLESS.hex`, or drop the `.hex` onto
the window. It is stored in `%APPDATA%\m8-live\firmware` and used from then on.

### Features

- Reference engine (Unicorn) running the original firmware; audio, display,
  keyboard and gamepads through SDL3
- Keys: arrows, Space = PLAY, X = EDIT, Z = OPTION, Shift = SHIFT
- `Documents\M8 Card` mirrors the SD card: edit it with the emulator closed
  (themes, songs, samples) and the next start applies it; songs you save
  appear there on exit
- The card sizes itself to its content (3-64 GB), as a sparse FAT32 file
- Drag sample folders onto the exe or the window to add them to the card
- External USB MIDI in and out, with hotplug
- Journaled card saves; a crashed session's work goes to
  `M8 Card (rescued DATE)`
- `m8emu.log` next to the exe, and a message box on any error

### Not yet

- Native/AOT fast engine (phase 2)
- MIDI SysEx
