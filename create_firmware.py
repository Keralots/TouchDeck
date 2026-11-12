#!/usr/bin/env python3
"""
Build TouchDeck and package the release images.

Usage:
    python create_firmware.py               # version from FW_VERSION in include/config.h
    python create_firmware.py v1.0.0        # explicit version
    python create_firmware.py --board p4-7b # one board only (default: all boards)
    python create_firmware.py --no-build    # package what .pio/build already has
    python create_firmware.py --flasher     # also publish the full images to the web flasher

Output (release/<version>/), <board> is empty for the S3 deck and "-p4-7b" for the 7B:
    TouchDeck-<version><board>-full.bin   bootloader + partitions + app, flash at 0x0
    TouchDeck-<version><board>-ota.bin    app only, for the portal's firmware update

With --flasher, docs/firmware/latest/ gets the full images, VERSION and SHA256SUMS.txt
(served by GitHub Pages at touchdeck.stolaris.dev).
"""

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

# The P4 ROM loads the bootloader from 0x2000, the S3 from 0x0.
BOARDS = {
    "s3":    {"env": "jc3248w535", "suffix": "",       "bootloader": 0x0},
    "p4-7b": {"env": "ws_p4_7b",   "suffix": "-p4-7b", "bootloader": 0x2000},
}
PARTITIONS_OFFSET = 0x8000
FIRMWARE_OFFSET = 0x10000

ROOT = Path(__file__).resolve().parent
FLASHER = ROOT / "docs" / "firmware" / "latest"


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


def build(env):
    pio = find_pio()
    if not pio:
        sys.exit("error: PlatformIO CLI not found (PATH or ~/.platformio/penv)")
    print(f"$ {pio} run -e {env}")
    if subprocess.run([pio, "run", "-e", env], cwd=ROOT).returncode != 0:
        sys.exit("error: build failed")


def merge_full(build_dir, bootloader_offset, out):
    parts = [build_dir / "bootloader.bin", build_dir / "partitions.bin", build_dir / "firmware.bin"]
    for p in parts:
        if not p.exists():
            sys.exit(f"error: {p} not found, build first")
    bl, pt, fw = (p.read_bytes() for p in parts)
    # The 0xFF fill also leaves otadata blank, so the bootloader starts app0.
    with open(out, "wb") as f:
        f.write(b"\xFF" * bootloader_offset)
        f.write(bl)
        f.write(b"\xFF" * (PARTITIONS_OFFSET - bootloader_offset - len(bl)))
        f.write(pt)
        f.write(b"\xFF" * (FIRMWARE_OFFSET - PARTITIONS_OFFSET - len(pt)))
        f.write(fw)


def publish_flasher(fulls, version):
    FLASHER.mkdir(parents=True, exist_ok=True)
    for old in FLASHER.glob("TouchDeck-*-full.bin"):
        old.unlink()
    sums = []
    for full in fulls:
        dst = FLASHER / full.name
        shutil.copy2(full, dst)
        sums.append(f"{hashlib.sha256(dst.read_bytes()).hexdigest()}  {dst.name}\n")
        print(f"  {dst.relative_to(ROOT)}  (web flasher, {version})")
    (FLASHER / "SHA256SUMS.txt").write_text("".join(sums), encoding="utf-8", newline="\n")
    (FLASHER / "VERSION").write_text(version + "\n", encoding="utf-8", newline="\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", nargs="?", default=None)
    ap.add_argument("--board", choices=sorted(BOARDS), action="append",
                    help="board to package, repeatable (default: all)")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--flasher", action="store_true")
    args = ap.parse_args()

    version = args.version or read_version()
    if not version:
        sys.exit("error: FW_VERSION not found in include/config.h; pass a version")
    if not version.startswith("v"):
        version = "v" + version
    boards = args.board or list(BOARDS)
    if args.flasher and set(boards) != set(BOARDS):
        sys.exit("error: --flasher needs every board, the page offers them all")

    out_dir = ROOT / "release" / version
    out_dir.mkdir(parents=True, exist_ok=True)
    fulls = []
    for name in boards:
        board = BOARDS[name]
        # One build at a time: parallel PlatformIO builds race on the shared cache.
        if not args.no_build:
            build(board["env"])
        build_dir = ROOT / ".pio" / "build" / board["env"]
        full = out_dir / f"TouchDeck-{version}{board['suffix']}-full.bin"
        ota = out_dir / f"TouchDeck-{version}{board['suffix']}-ota.bin"
        merge_full(build_dir, board["bootloader"], full)
        shutil.copy2(build_dir / "firmware.bin", ota)
        for p in (full, ota):
            print(f"  {p.relative_to(ROOT)}  ({p.stat().st_size / 1024:.1f} KB)")
        fulls.append(full)
    if args.flasher:
        publish_flasher(fulls, version)


if __name__ == "__main__":
    main()
