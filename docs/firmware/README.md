# Flasher firmware

`latest/` holds:

- `VERSION`: one line with the release version, for example `v1.2.0`.
- `TouchDeck-<VERSION>-boot.bin`: the Guition JC3248W535 (ESP32-S3) bootloader and partition table, flashed at `0x0`, up to the NVS partition.
- `TouchDeck-<VERSION>-app.bin`: blank otadata and the app, flashed at `0xe000`.
- `TouchDeck-<VERSION>-p4-7b-boot.bin` / `-p4-7b-app.bin`: the same for the Waveshare P4 7B (ESP32-P4 v3.x); its boot part starts at `0x2000`.
- `SHA256SUMS.txt`: the firmware checksums.

The parts skip NVS (`0x9000..0xe000`), so an install keeps the settings unless Erase device is picked. Blank otadata makes the bootloader start app0 even after an OTA moved it to app1.

The installer picks the parts by the chip it detects. To publish a release, run `create_firmware.py --flasher`; it cuts the parts from the full images and writes `VERSION` and `SHA256SUMS.txt`. `create_firmware.py <version> --republish` does the same from `release/<version>/` without a build. The page reads `VERSION` and builds the paths automatically, so it needs no code change.
