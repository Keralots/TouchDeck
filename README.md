# TouchDeck

A touch control deck for the Guition JC3248W535 (3.5" 320x480 IPS, capacitive
touch, ESP32-S3 with 16 MB flash and 8 MB PSRAM). Pages of tiles switch and
read Home Assistant entities and show live PC stats from the StatNode
Companion. Everything is set up from a web portal on the device.

No cloud, no accounts: the device talks to Home Assistant over its local
WebSocket API and receives PC metrics over UDP on the LAN.

## What it does

- **Pages of tiles** in a grid (up to 6x4 per page): switches, buttons and
  sensors from Home Assistant, and PC metric tiles (number, chart or bar).
- **Columns:** pages stacked into a column. Swipe left and right between
  columns, up and down between the pages of a column.
- **Home Assistant:** live state over the WebSocket API, optimistic toggles,
  scenes / scripts / buttons, a detail card on long press with a brightness
  slider for lights. Icons come from Material Design Icons, the set Home
  Assistant uses (the font is installed from the portal).
- **PC stats:** metric tiles fed by the StatNode Companion; values, units,
  history chart and an amber warning colour above a set share of the scale.
- **Quick panel:** pull down from the top edge for screen and LED brightness,
  link status, screen off, night mode, the top bar switch and the page editor.
- **Edit on the device:** long press an empty spot of a page (or Edit in the
  quick panel) to move, resize or remove its tiles; Done saves the layout.
- **Night schedule** for the backlight and an optional status / filament LED
  on a PWM pin.
- **System screens** for start-up, WiFi setup (with a QR code to join the
  setup network), connecting, and a firmware update screen with progress.

## Web portal

Open the device IP (or `http://<hostname>.local`) in a browser:

- **Overview:** live picture of the screen, PC stream, Home Assistant link,
  diagnostics.
- **Pages & tiles:** Home Assistant address and token, the icon font, pages,
  tiles, entity and icon pickers, columns, and a grid editor: drag tiles to
  move or swap them, resize from the corner, separately for landscape and
  portrait.
- **Display:** rotation, brightness, night schedule, PC metric tile scales.
- **Time:** 12/24 h clock and timezone.
- **Hardware:** the status LED.
- **Network:** WiFi, hostname / mDNS, static IP.
- **Maintenance:** configuration backup (pages and settings; WiFi and the Home
  Assistant token are never included), firmware update, factory reset.

## Home Assistant

In **Pages & tiles**, enter the Home Assistant address (for example
`http://192.168.0.7:8123`) and a long-lived access token (HA profile,
Security tab). The token is stored only on the device. Then add tiles from the
entity picker.

## PC stats: StatNode Companion

The companion runs on the PC, reads the sensors and pushes a JSON packet to
UDP port `4210` of the device every second. A copy of it lives in
[`StatNode-CompanionApp/`](StatNode-CompanionApp/) (it is shared with the
StatNode project).

- **Windows:** reads [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor),
  which must be running (as administrator for full coverage), with its remote
  web server enabled (port `8085`) or through WMI.
- **Linux:** reads the sensors directly.

The companion UI opens at `http://127.0.0.1:8740`; point it at the device IP.
The PC and the device must share a subnet with UDP `4210` reachable. Add metric
tiles in **Pages & tiles** with "+ Add PC metric".

## Hardware

Guition JC3248W535: AXS15231B panel over QSPI, AXS15231B touch on I2C. Nothing
to wire for the panel or touch.

Optional status LED: a plain single-colour LED driven through a transistor
from a free GPIO (default 17 on the extended IO header). Pins used by the
display, touch, USB, flash or PSRAM are refused by the portal.

## Build and flash

Built with [PlatformIO](https://platformio.org/), one environment:

```sh
pio run                       # build
pio run -t upload             # flash over USB
curl -F "firmware=@.pio/build/jc3248w535/firmware.bin" http://<device-ip>/ota/upload
python create_firmware.py     # full and OTA images in release/<version>/
```

The full image is flashed at `0x0` (esptool or the
[ESP web flasher](https://espressif.github.io/esptool-js/) in Chrome or Edge);
the OTA image is uploaded in **Maintenance > Firmware update**.

## First-run setup

1. Flash the firmware and power the device.
2. It opens the WiFi network **`TouchDeck-XXXX`** (password `touchdeck1234`);
   scan the QR code on the screen or join it by hand, and the setup page opens
   at `http://192.168.4.1`. (Improv over USB serial also works for the first
   3 minutes.)
3. Once it is on your network, open the device IP, connect Home Assistant and
   build your pages.

## Credits

Built on [LVGL](https://lvgl.io/),
[ArduinoJson](https://github.com/bblanchon/ArduinoJson),
[arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets),
[GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX)
(AXS15231B init table) and [Improv WiFi](https://www.improv-wifi.com/)
(vendored under `lib/ImprovWiFi`). Fonts: Montserrat (SIL OFL 1.1) with the
LVGL symbol set (Font Awesome Free); icons: Material Design Icons, installed on
the device by the user.
