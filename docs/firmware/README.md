# Flasher firmware

`latest/` holds:

- `VERSION`: one line with the release version, for example `v1.2.0`.
- `TouchDeck-<VERSION>-full.bin`: the merged bootloader, partitions and app image flashed at `0x0`.
- `SHA256SUMS.txt`: the firmware checksum.

To publish a release, replace the `.bin`, `VERSION` and `SHA256SUMS.txt`. Running `create_firmware.py --flasher` does this. The page reads `VERSION` and builds the image path automatically, so it needs no code change.
