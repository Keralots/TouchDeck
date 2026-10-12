#!/usr/bin/env python3
"""
Build TouchDeck and package the release images.

Usage:
    python create_firmware.py               # version from FW_VERSION in include/config.h
    python create_firmware.py v1.0.0        # explicit version
    python create_firmware.py --no-build    # package what .pio/build already has

Output (release/<version>/):
    TouchDeck-<version>-full.bin   bootloader + partitions + app, flash at 0x0
    TouchDeck-<version>-ota.bin    app only, for the portal's firmware update
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ENV = "jc3248w535"
BOOTLOADER_OFFSET = 0x0
PARTITIONS_OFFSET = 0x8000
FIRMWARE_OFFSET = 0x10000

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / ".pio" / "build" / ENV


def read_version():
    text = (ROOT / "include" / "config.h").read_text(encoding="utf-8")
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', text)
    return "v" + m.group(1) if m else None


def find_pio():
    for name in ("pio", "platformio"):
        found = shutil.which(name)
        if found:
            return found
    for c in (Path.home() / ".platformio/penv/Scripts/pio.exe",
              Path.home() / ".platformio/penv/bin/pio"):
        if c.exists():
            return str(c)
    return None


def build():
    pio = find_pio()
    if not pio:
        sys.exit("error: PlatformIO CLI not found (PATH or ~/.platformio/penv)")
    print(f"$ {pio} run -e {ENV}")
    if subprocess.run([pio, "run", "-e", ENV], cwd=ROOT).returncode != 0:
        sys.exit("error: build failed")


def merge_full(out):
    parts = [BUILD / "bootloader.bin", BUILD / "partitions.bin", BUILD / "firmware.bin"]
    for p in parts:
        if not p.exists():
            sys.exit(f"error: {p} not found, build first")
    bl, pt, fw = (p.read_bytes() for p in parts)
    # The 0xFF fill also leaves otadata blank, so the bootloader starts app0.
    with open(out, "wb") as f:
        f.write(bl)
        f.write(b"\xFF" * (PARTITIONS_OFFSET - len(bl)))
        f.write(pt)
        f.write(b"\xFF" * (FIRMWARE_OFFSET - PARTITIONS_OFFSET - len(pt)))
        f.write(fw)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", nargs="?", default=None)
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    version = args.version or read_version()
    if not version:
        sys.exit("error: FW_VERSION not found in include/config.h; pass a version")
    if not version.startswith("v"):
        version = "v" + version

    if not args.no_build:
        build()

    out_dir = ROOT / "release" / version
    out_dir.mkdir(parents=True, exist_ok=True)
    full = out_dir / f"TouchDeck-{version}-full.bin"
    ota = out_dir / f"TouchDeck-{version}-ota.bin"
    merge_full(full)
    shutil.copy2(BUILD / "firmware.bin", ota)
    for p in (full, ota):
        print(f"  {p.relative_to(ROOT)}  ({p.stat().st_size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
