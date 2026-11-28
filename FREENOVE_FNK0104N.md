# Draft port: Freenove ESP32-S3 3.5" Display (FNK0104N)

This is a **community draft**, not an officially supported board. TouchDeck's
browser flasher only knows the Guition JC3248W535 and Waveshare P4 7B; flashing
either image onto the Freenove board gives a black screen (different panel,
different pins). You have to build and flash this environment yourself.

## Build and flash

```sh
pio run -e freenove_fnk0104n
```

PlatformIO first downloads the pinned pioarduino platform and toolchain (~1 GB).
The build produces `firmware.factory.bin`, a combined image containing the
bootloader, partition table, boot_app0 and application image. Flash that single
file at offset 0x0:

```sh
& "$env:USERPROFILE\.platformio-pioarduino\penv\Scripts\python.exe" -m esptool `
  --chip esp32s3 --port COM9 --baud 921600 write_flash 0x0 `
  .pio\build\freenove_fnk0104n\firmware.factory.bin
```

**Do not use `pio run -t upload` here.** Its Tool Manager step re-installs
`tool-esptoolpy` (5.6.0), which changes the SCons tool signature and throws away
the whole build cache — a full rebuild takes about 65 minutes. If you do run it
by accident, let it finish rather than killing it: stopping it mid-build deletes
the finished `firmware.factory.bin` and forces another full rebuild. To flash
only the application later, write `firmware.bin` at 0x10000 — this keeps NVS
and saved Wi-Fi credentials:

```sh
& "$env:USERPROFILE\.platformio-pioarduino\penv\Scripts\python.exe" -m esptool `
  --chip esp32s3 --port COM9 --baud 921600 write_flash 0x10000 `
  .pio\build\freenove_fnk0104n\firmware.bin
```

Never write `firmware.bin` alone at 0x0; use `firmware.factory.bin` there.

If the board stops booting after a bad image: hold **BOOT**, tap **RESET**,
release BOOT, then flash again. USB is on GPIO 19/20 (CDC on boot).

### Verified on hardware

The draft was built and flashed on COM9 (VID 303A:1001, ESP32-S3 rev v0.2)
and booted successfully. Key log lines:

```
TouchDeck 1.5.0 boot
Deck panel: up, rotation 1, internal free 137816, largest 126964, PSRAM free 7771408
Deck: config from built-in, 3 pages
Deck: LVGL 9.5.0 up, 2 pages, 480x320
Touch: ST77922 fw 3, 5 touch points
AP started: TouchDeck-68CC (IP: 192.168.4.1)
Web server started on :80
pc_metrics: UDP listening on port 4210
Deck: frames=3 comp=0 flush=8336/8748us build=6893us te_late=0 te_to=0 lv_drop=0 ...
```

`NVS NOT_FOUND` messages on the first boot are expected — no settings are
saved yet. The low `frames` count is normal on the static Wi-Fi setup screen:
LVGL only redraws when something changes. `te_late=0` and `te_to=0` mean the
software vertical-blank timer is keeping up. The chat about the chip being
stuck in `DOWNLOAD(USB/UART0)` was a side effect of the host toggling DTR/RTS
while probing the port; a normal power-on boots the application fine.

After Wi-Fi setup, the board was verified on the live panel: Wi-Fi STA
connected (`ME`), weather/NTP data arrived, tiles and the clock/timer card
respond to taps (including all card buttons and Close), horizontal swipes
switch pages and a downward swipe from the top edge opens the quick panel.
Touch down/up events arrive in pairs; no stuck contact was observed. Audio was
verified too: after enabling it in the portal, a test tone and the bell of a
10-second countdown both played through the speaker (log: `Deck: timer
finished`). ES8311 init reported no errors and the active-low amp on GPIO 1
works.

Touch root cause found during testing: the controller reports five contact
slots, but the driver read only the first 7 of 35 bytes. The flag signalling a
new sample stayed set, so the release sample never reached the app and a touch
appeared to stick after the first tap. Reading the full buffer (and keeping
the vendor's INFO/STATUS-before-points sequence) fixes taps and swipes. Flashing
only `firmware.bin` at 0x10000 preserves NVS/Wi-Fi across later updates; a full
`firmware.factory.bin` flash at 0x0 erases the saved credentials.

## What the port changes

| Piece | Implementation |
| --- | --- |
| Board definition | `include/boards/freenove_fnk0104n.h` (pins, grid, audio, SD, reserved pins) |
| Build target | `[env:freenove_fnk0104n]` in `platformio.ini` (S3, N16R8, qio_opi, 16 MB) |
| Panel | ST77922 init table from Freenove's `ST77922.cpp`, verbatim; QSPI at 80 MHz, 48 native rows per transaction, RGB565, software rotation |
| Sync | No TE pin on this board: a periodic `esp_timer` at 16390 µs acts as the vertical-blank edge |
| Touch | `src/deck_touch_st77922.cpp`: I2C 0x55, 16-bit registers, polled; reads INFO/STATUS then **all** reported contact slots (7 bytes each), last contact cached between NEW samples |
| Audio | Existing ES8311 backend with amp enable GPIO 1, active low (internal pull-down on boot) |
| Card | 4-bit SDMMC slot 0 on GPIO 2–7, no SPI fallback |

## First things to check on hardware

1. **Display:** the boot splash appears, colors and orientation are correct in
   all four rotations. The panel is inverted in the vendor init (`INVON`);
   swap `0x21` for `0x20` in the init table if the colors look negative.
2. **Serial log:** `Deck: frames=... te_late=...` should show a steady stream.
   A high `te_late` means the 80 MHz bus is not keeping up; drop `SPI_HZ` to
   40000000 in `src/deck_panel_qspi.cpp` and rebuild.
3. **Tearing:** there is no TE line, so frames are not locked to the scan. If
   tearing is objectionable, the timer can be shifted with
   `PANEL_SYNC_PERIOD_US` / `TE_LATE_US` in `deck_panel_int.h` and
   `deck_panel_qspi.cpp`, but it cannot be eliminated in software.
4. **Touch:** tap and swipe in every rotation. The driver reads every reported
   contact slot, so a touch that "sticks" after one tap means the buffer read
   was truncated again (or the INFO/STATUS reads were removed) — compare with
   `src/deck_touch_st77922.cpp`.
5. **Audio:** sound is **off by default** (`audio.enabled = false` in
   `/api/config`), so a finished timer is silent until you enable Audio in the
   portal's hardware settings and set a volume. The `timer` event is on by
   default; `nightMute` can silence it at night. The amp is enabled by the
   ES8311 backend. The internal microphone/agent page is compiled out (no
   ES7210 driver on this board).
6. **SD card:** a FAT card should mount in 4-bit mode. One-bit/SPI fallback is
   not configured.

## Notes

- The flash/PSRAM pins (26–37), USB (19/20), QSPI display, I2C, I2S and SD pins
  are marked reserved, so the LED/vibration-module picker will not claim them.
- The on-board WS2812 (GPIO 40) and battery sense are not wired into TouchDeck
  by this draft.
- The QSPI clock is the vendor's validated 80 MHz; if the image is corrupted,
  lower it before investigating anything else.
- This port is uncommitted work in the local tree; rebase onto newer upstream
  releases before submitting it as a PR.
