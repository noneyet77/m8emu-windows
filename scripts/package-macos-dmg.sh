#!/bin/sh
# Wrap m8emu.app in a drag-to-Applications disk image, after checking that the
# app is self-contained (no Homebrew or other outside libraries) and holds no
# firmware.
# Usage: sh scripts/package-macos-dmg.sh <m8emu.app> <out.dmg> <version>
set -eu
app=$1 out=$2 version=$3
[ -d "$app/Contents/MacOS" ] || { echo "no app at $app" >&2; exit 1; }

# Every library the program and its bundled libraries load must come from
# the app itself or from macOS.
bad=0
for bin in "$app/Contents/MacOS/m8emu" "$app"/Contents/Frameworks/*.dylib; do
  [ -f "$bin" ] || continue
  otool -L "$bin" | tail -n +2 | awk '{print $1}' | while read -r lib; do
    case "$lib" in
      @rpath/*|@executable_path/*|@loader_path/*|/usr/lib/*|/System/*) ;;
      *) echo "outside library: $bin -> $lib" >&2; echo bad ;;
    esac
  done | grep -q bad && bad=1
done
[ "$bad" = 0 ] || { echo "app is not self-contained" >&2; exit 1; }

if find "$app" \( -iname '*.hex' -o -iname '*.bin' -o -name 'libm8aot*' -o -name 'libm8native*' \) | grep -q .; then
  echo "firmware or firmware-derived file inside the app" >&2; exit 1
fi
codesign --verify --deep --strict "$app"

stage=$(mktemp -d)
ditto "$app" "$stage/m8emu.app"
ln -s /Applications "$stage/Applications"
cat > "$stage/First launch.txt" <<NOTE
m8emu $version for macOS (Apple Silicon, macOS 13 or newer)

1. Drag m8emu onto Applications.
2. The app is not notarized by Apple, so macOS blocks the first launch.
   Open Terminal and run once:

     xattr -dr com.apple.quarantine /Applications/m8emu.app

   (Or: try to open it, then System Settings > Privacy & Security >
   Open Anyway.)
3. The M8 Headless firmware is not included. Get M8_V6_5_2C_HEADLESS.hex
   free from Dirtywave, then pick it in the app or drop it on the window.
NOTE

mkdir -p "$(dirname "$out")"
hdiutil create -volname "m8emu $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$out"
hdiutil verify "$out"
rm -rf "$stage"
ls -l "$out"
