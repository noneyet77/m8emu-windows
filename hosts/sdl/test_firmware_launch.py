#!/usr/bin/env python3
"""Consecutive launches of a firmware-free m8emu with a temporary HOME.

1. No firmware: the firmware screen opens; the official HEX is dropped on it,
   verified, stored, and the guest boots.
2. Relaunch: the stored copy is re-verified and the guest boots directly.
3. A user-built engine in engine/<sha>/ loads automatically (with --engine-lib).
4. A damaged stored copy is refused: headless fails clearly, the window asks again.
Video is SDL's dummy driver and audio the simulated consumer; nothing plays.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BIN_SHA256 = "20498b7637cee17baae249372db3b8377afd4e12a5e7062754c2ddd83621367f"
SUFFIX = ".dylib" if sys.platform == "darwin" else ".so"
ENGINE = ("libm8aot", "libm8native-step", "libm8native-regions")


def run(binary: str, home: Path, *args: str, window: bool = True) -> tuple[int, str]:
    env = {k: v for k, v in os.environ.items() if k != "XDG_DATA_HOME" and not k.startswith("M8")}
    env.update(HOME=str(home), SDL_AUDIO_DRIVER="dummy")
    if window:
        env.update(SDL_VIDEO_DRIVER="dummy", SDL_RENDER_DRIVER="software")
    command = [binary, "--demo", "simple", "--seconds", "2", *args]
    if window:
        command += ["--consumer", "simulated"]
    try:
        done = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode() if isinstance(exc.stdout, bytes) else exc.stdout or ""
        return -1, out + "\n<timeout: the app waited for input>"
    return done.returncode, done.stdout + done.stderr


def expect(condition: bool, label: str, output: str) -> None:
    print(("PASS " if condition else "FAIL ") + label)
    if not condition:
        print(output[-3000:])
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="m8emu without firmware/ or lib/ beside it")
    parser.add_argument("--hex", required=True, help="Dirtywave's M8_V6_5_2C_HEADLESS.hex")
    parser.add_argument("--engine-lib", help="folder holding the three generated engine libraries")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="m8-launch-") as temporary:
        home = Path(temporary)
        data = home / ("Library/Application Support" if sys.platform == "darwin" else ".local/share") / "m8-live"
        stored = data / "firmware" / "M8_V6_5_2C_HEADLESS.bin"

        code, out = run(args.binary, home, "--picker-smoke", args.hex)
        expect(code == 0 and "firmware_picker=shown reason=missing" in out, "launch 1 shows the firmware screen", out)
        expect("status=ok" in out and "firmware_source=picker" in out, "launch 1 installs the dropped HEX and boots", out)
        expect(stored.is_file() and hashlib.sha256(stored.read_bytes()).hexdigest() == BIN_SHA256,
               "launch 1 stores the verified binary", out)

        code, out = run(args.binary, home)
        expect(code == 0 and "firmware_picker" not in out and "firmware_source=user" in out,
               "launch 2 boots the stored firmware without the screen", out)
        expect("engine=reference engine_source=none" in out, "launch 2 runs the reference engine", out)

        if args.engine_lib:
            engine = data / "engine" / BIN_SHA256
            engine.mkdir(parents=True)
            for name in ENGINE:
                shutil.copy2(Path(args.engine_lib) / (name + SUFFIX), engine / (name + SUFFIX))
            code, out = run(args.binary, home)
            expect(code == 0 and "firmware_picker" not in out and "engine=native engine_source=user" in out,
                   "launch 3 loads the user-built engine", out)
            shutil.rmtree(data / "engine")

        image = bytearray(stored.read_bytes())
        image[0x1000] ^= 0xff
        stored.write_bytes(image)
        code, out = run(args.binary, home, "--headless", window=False)
        expect(code != 0 and "failed its SHA-256 check" in out, "a damaged copy fails headless launches", out)
        code, out = run(args.binary, home, "--picker-smoke", args.hex)
        expect(code == 0 and "firmware_picker=shown reason=invalid" in out and "status=ok" in out,
               "a damaged copy brings the screen back and is replaced", out)
        expect(hashlib.sha256(stored.read_bytes()).hexdigest() == BIN_SHA256, "the replacement is verified", out)
    print("firmware launch test passed")


if __name__ == "__main__":
    main()
