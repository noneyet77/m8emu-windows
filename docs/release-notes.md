## m8emu 0.1.2 for Windows and macOS

Two downloads, same version:

- **Windows:** `m8emu-0.1.2-windows-x64.exe`, the whole app in one file.
  Windows 10 (1903) or newer, x64.
- **macOS:** `m8emu-0.1.2-macos-arm64.dmg`, with a self-contained
  `m8emu.app` inside. Apple Silicon, macOS 13 or newer.

The M8 Headless firmware is **not included** in either. Get
`M8_V6_5_2C_HEADLESS.hex` free from Dirtywave, then pick it in the app
(**CHOOSE FILE...**) or drop it on the window. It is kept from then on.

### First launch

- **Windows:** SmartScreen says "Windows protected your PC" because the exe
  isn't code-signed. Click **More info**, then **Run anyway**.
- **macOS:** open the dmg and drag m8emu onto Applications. The app isn't
  notarized, so run this once in Terminal before the first launch:
  `xattr -dr com.apple.quarantine /Applications/m8emu.app`
  (or try to open it, then System Settings > Privacy & Security >
  **Open Anyway**).

### New in 0.1.2

- macOS build added to the release, built and packaged automatically
  alongside the Windows exe.
- The macOS app runs the reference engine out of the box; F4 opens the
  firmware screen, where it can build the faster native engine on your Mac.

### Windows

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
- Vsync on by default (fixes flicker during playback); `--vsync 0` turns
  it off
- `m8emu.log` next to the exe, and a message box on any error

### Not yet

- The Windows-only extras above (M8 Card folder, self-sizing card, USB MIDI,
  drag-and-drop import) on macOS
- Native/AOT fast engine on Windows
- MIDI SysEx
