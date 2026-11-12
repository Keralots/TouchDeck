# Flasher firmware

`latest/` holds:

- `VERSION`: one line with the release version, for example `v1.2.0`.
- `TouchDeck-<VERSION>-full.bin`: the Guition JC3248W535 (ESP32-S3) image, bootloader, partitions and app merged, flashed at `0x0`.
- `TouchDeck-<VERSION>-p4-7b-full.bin`: the same for the Waveshare P4 7B (ESP32-P4 v3.x); its bootloader sits at `0x2000` inside the image.
- `SHA256SUMS.txt`: the firmware checksums.

The installer picks the image by the chip it detects. To publish a release, replace the `.bin` files, `VERSION` and `SHA256SUMS.txt`. Running `create_firmware.py --flasher` does this. The page reads `VERSION` and builds the image path automatically, so it needs no code change.
