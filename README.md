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
  Tiles are placed and sized in half-cell steps (1x1.5, 1.5x1, 0.5x0.5...);
  text and icons scale with the tile, so small tiles shrink instead of
  overlapping.
- **Columns:** pages stacked into a column. Swipe left and right between
  columns, up and down between the pages of a column.
- **Home Assistant:** live state over the WebSocket API, optimistic toggles,
  scenes / scripts / buttons, a detail card on long press with a level
  slider (light brightness, cover position, fan speed, volume, numbers).
  Icons come from Material Design Icons, the set Home Assistant uses (the
  font is installed from the portal).
- **Sensor history:** `style: "chart"` and `hours` (1..168, default 24) show
  Home Assistant history; the detail card includes min / max / now. Optional
  `warn` and `crit` thresholds colour the value and line amber or red;
  `crit` below `warn` makes lower values worse, such as battery levels.
- **Slider tiles:** a light, cover, fan, media player or number as a tile
  that fills to its level; drag sideways on it to change the level (the page
  does not swipe there), tap to toggle, hold for the card. Pick "Slider" as
  the tile type in the portal.
- **Confirm before acting:** a tile can ask on the device before it turns
  something off (a printer mid-print) or before every action. Set it under
  the tile's "more options" button in the portal.
- **HTTP buttons:** a button can send an HTTP request to a device on the LAN
  (Tasmota, Node-RED, your own projects) instead of calling Home Assistant.
- **PC stats:** metric tiles fed by the StatNode Companion; values, units,
  history chart and an amber warning colour above a set share of the scale.
- **Quick panel:** pull down from the top edge for screen and LED brightness,
  link status, screen off, night mode, the top bar switch and the page editor.
- **Edit on the device:** long press an empty spot of a page (or Edit in the
  quick panel) to move, resize or remove its tiles, or add one with + (a
  list of Home Assistant entities by group, from the list cached with
  "Update from HA" in the portal); Done saves the page.
- **Night schedule** for the backlight and an optional status / filament LED
  on a PWM pin. The LED's day brightness can follow a Home Assistant
  input_boolean, input_number or light, and the quick panel's Light slider
  sets that entity.
- **System screens** for start-up, WiFi setup (with a QR code to join the
  setup network), connecting, and a firmware update screen with progress.

## Web portal

Open the device IP (or `http://<hostname>.local`) in a browser:

- **Overview:** live picture of the screen, PC stream, Home Assistant link,
  diagnostics.
- **Pages & tiles:** Home Assistant address and token, the icon font, pages,
  tiles, entity and icon pickers, columns, and a grid editor: drag tiles to
  move or swap them, resize from the corner (half-cell steps), separately for landscape and
  portrait.
- **Display:** rotation, theme (Midnight, Daylight, or Glass: tinted glass
  tiles over a wallpaper you upload), brightness, night
  schedule, PC metric tile scales.
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

## HTTP buttons

A button tile can send an HTTP request instead of calling Home Assistant, for
devices and services on the LAN that take commands over plain http (Tasmota,
Node-RED, your own ESP projects). In **Pages & tiles**, use "+ Add HTTP
button", or open a button's "more options" and set "On tap" to "HTTP
request". Fill in the method, URL and optionally a body and headers; "Test"
sends the request from the device and shows the answer. On the device a tap
shows a short toast: "OK" for a 2xx answer, otherwise the status code or why
it failed.

Example, toggling a Tasmota relay
([Tasmota docs](https://tasmota.github.io/docs/Commands/), spaces as `%20`):

```json
{ "id": "plug", "type": "action", "label": "Plug", "icon": "mdi:power",
  "confirm": "always",
  "tap": { "action": "http", "url": "http://192.168.0.50/cm?cmnd=Power%20Toggle" } }
```

The full action: `"method"` (GET, POST, PUT, PATCH, DELETE; GET by default,
POST when there is a body), `"url"` (http only, no https), `"body"` (text,
or a JSON object), `"contentType"` (automatic: a body starting with `{` or `[`
is sent as application/json, other text as text/plain) and `"headers"`
(`{ "Name": "value" }`). The request lives in the deck configuration, which
anyone on your network can read through the portal, so keep important
passwords out of it.

## HTTP sensors

Use "+ Add HTTP sensor" to poll a LAN JSON endpoint and show one value.
Test fetches the reply from the device; click a JSON leaf to select its path.
For example, a PC endpoint returning `{"claude":{"five_hour":{"used":18}}}`:

```json
{ "type": "sensor", "label": "Claude 5h", "icon": "mdi:robot",
  "http": { "url": "http://192.168.0.10:8760/usage",
            "path": "claude.five_hour.used", "poll": 60 },
  "unit": "%", "style": "chart", "warn": 70, "crit": 90 }
```

`http.url` uses plain http and GET only. `http.path` selects dot-separated
keys; numeric segments index arrays (`items.0.value`). An empty path reads a
bare number or string. `http.poll` is seconds, 5..3600, default 30. Optional
`http.headers` is a `{"Name":"value"}` object, with the same visibility as
HTTP button headers. Tasmota's `http://<ip>/cm?cmnd=<command>` URL form is
another possible source; choose a path from that command's actual reply.

Optional `unit` is appended only to JSON numbers, formatted with at most one
decimal. Strings display as received, booleans as On/Off. `style` is `number`
or `chart`; charts keep the last 48 numeric readings since boot. `warn` and
`crit` use the same numeric thresholds as HA sensors, including lower-is-worse.
Tiles sharing a URL and headers share one request at their shortest poll
interval. A missing path shows `--`; failed first polls or replies older than
three poll intervals show `--` at half opacity. Polling runs only with WiFi,
with a 32 KB response limit. Tiles are read-only by default; explicit `tap`
and `hold` actions still work, without a default HA card.

## PC stats and control: TouchDeck Companion

The companion runs on the PC, reads the sensors and pushes a JSON packet to
UDP port `4210` of the device every second. It lives in
[`TouchDeck-CompanionApp/`](TouchDeck-CompanionApp/) (forked from the StatNode
companion; it runs side by side with the StatNode, SmallOLED and
AnimatedPixelClock companions).

- **Windows:** reads [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor),
  which must be running (as administrator for full coverage), with its remote
  web server enabled (port `8085`) or through WMI.
- **Linux:** reads the sensors directly.

The companion UI opens at `http://127.0.0.1:8750`; point it at the device IP.
The PC and the device must share a subnet with UDP `4210` reachable. Add metric
tiles in **Pages & tiles** with "+ Add PC metric".

- **Several PCs:** each companion sends its own id and name (Connection page,
  "This PC's name"); the device keeps up to four PCs and a metric tile picks
  one.
- **Control:** on the companion's Device page, "Pair for control" shows a code
  on the device; type it in. Tiles made with "+ Add PC control" then send
  media keys, volume, lock, sleep, restart or shut down to that PC (Sleep,
  Restart and Shut down ask on the device first). Commands are signed with
  the key from pairing and arrive on UDP `4211` (allow it in the firewall).
- **USB cable:** when the device is plugged into the PC, the companion finds
  it (Espressif USB serial, 303A:1001) and sends metrics and commands over the
  cable instead of WiFi. While it holds the port, USB flashing and the serial
  monitor cannot open it: use OTA, or set "USB cable" to Off.

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

## License

MIT, see [LICENSE](LICENSE). The vendored Improv WiFi library keeps its own
MIT license in `lib/ImprovWiFi/LICENSE`.
