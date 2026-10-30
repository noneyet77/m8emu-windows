# Windows (phase 1: reference engine)

Private port. Runs the original firmware through Unicorn (the "reference"
engine). The native/AOT engine and PGO are not ported yet.

## Build on Windows with MSYS2

1. Install MSYS2 from https://www.msys2.org and open the **MSYS2 MINGW64** shell.
2. Install the toolchain:

   ```sh
   pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-python
   ```

3. Clone with submodules and build:

   ```sh
   git clone --recursive <your private repo URL> m8emu
   cd m8emu
   cmake --preset windows && cmake --build --preset windows
   ```

4. The app is `build/windows/m8emu.exe`. The DLLs and `firmware/` beside it
   must stay in the same folder. Run it from that folder or double-click it.

## Single-executable release

`cmake --preset windows-release && cmake --build --preset windows-release`
(or `windows-release-cross` from Linux) links everything statically into one
`m8emu.exe` with no DLLs, the gamepad database and version info embedded.
The Headless firmware is never part of it: on first run the app asks for the
`.hex` from Dirtywave. Pushing a tag such as `v0.1.0` runs
`.github/workflows/windows-release.yml`, which builds that exe and attaches
it to the GitHub release with `docs/release-notes.md` as its text.

## Cross-build from Linux

Install `mingw-w64` (posix threads variant), `cmake` and `ninja`, then:

```sh
cmake --preset windows-cross && cmake --build --preset windows-cross
```

## Where things live

- Default card: `%APPDATA%\m8-live\card.img` (plus `.bak`, `.lock` and
  `.settings.nor` beside it). Use `--card PATH` for another card.
- Browse the card: `Documents\M8 Card` is a normal folder that mirrors the
  card. With the emulator closed, change anything there (rearrange themes,
  back up or delete songs, add samples); the next start puts the card in
  exactly that state, deletions included. On exit, only what the M8 changed
  is copied back into the folder. If you edit the folder while the emulator
  is running, your edits are kept and this session's card goes to a separate
  `M8 Card (emulator copy DATE)` folder instead. `--card-folder DIR` picks
  another folder; `--no-card-folder` turns the mirror off (and goes back to
  the original 128 MiB card).
- The card sizes itself to what is on it: your content plus 2 GB free for
  new songs and renders, in whole GB (at least 3 GB, at most 64 GB). It is
  checked on every start; growing or shrinking is instant. It is a sparse
  FAT32 file, so it only takes disk space for what is on it, and it is
  loaded lazily, so a big card starts as fast as a small one. On exit only
  the parts the M8 wrote are saved, through a journal that is finished
  automatically if the PC loses power mid-save. When the card is rebuilt from
  the folder (after you change the folder), the previous card is kept as
  `card.img.bak`.
- Add samples without commands: drag folders onto `m8emu.exe` (or a
  shortcut to it), or onto the running M8 window. A folder goes to
  `/Samples/<folder name>`; a folder that already holds card folders
  (Samples, Songs, ...) merges at the card root. Dropping on the running
  window asks first, then saves the card and restarts the M8 with the
  import (save your song first). Identical files already on the card are
  skipped; a different file with the same name is refused.
- If the M8 crashes, the main card and `M8 Card` folder are left as they
  were, and anything the session wrote (songs you saved) is copied to
  `M8 Card (rescued DATE)`. A stray memory read by the firmware no longer
  ends the session; it is logged as `guest_warning` in `m8emu.log`.
- External MIDI: every USB MIDI keyboard, controller or interface is
  connected to the M8's MIDI input (notes, CCs, clock and transport;
  SysEx is not routed), and what the M8 sends goes to every MIDI output
  except Windows' built-in GS synth. Devices can be plugged in while the
  emulator runs (picked up within two seconds). `--midi-in NAME` /
  `--midi-out NAME` pick devices by name, `none` turns a direction off,
  `--no-midi` turns it all off. The log lists the devices opened. A device
  another program (a DAW) has open cannot be shared; close it there first.
- Keys: arrows, Space = PLAY, X = EDIT, Z = OPTION, Shift = SHIFT
  (Ctrl and Alt also work as EDIT and OPTION). F4 opens the firmware screen.
  Keys live in `%APPDATA%\m8c\config.ini`.
- From a terminal: `m8emu.exe --card PATH --import FOLDER` (card root;
  add `--overwrite` to replace files), `m8emu.exe --card PATH export FOLDER`.
- Save in the M8 (SAVE/BUNDLE) and close the window normally; the card is
  published on clean exit only.

## Windows-specific notes

- Storage uses wide Win32 APIs, so non-ASCII sample names work. Links,
  junctions and other reparse points are refused, like symlinks on POSIX.
- Other paths (for example `--firmware`) rely on the UTF-8 code page in the
  embedded manifest, which needs Windows 10 version 1903 or newer.
- Double-clicking opens no console. Output goes to `m8emu.log` beside the
  exe (or `%APPDATA%\m8-live\m8emu.log` if that folder is read-only), and
  any error or crash shows a message box with the error and the log path.
- Run from a terminal (`cmd` or PowerShell), output prints there instead.
  The prompt may return before the app exits; use `start /wait m8emu.exe`
  to keep it in order.
- If audio crackles, try `--queue-ms 40` (larger buffer, more latency).
