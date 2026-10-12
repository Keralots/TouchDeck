# TouchDeck Companion for Linux

The Linux companion shares the TouchDeck web UI and UDP sender with the
Windows edition while using the Linux sensor backend.

## Run

```sh
python3 -m pip install -r requirements.txt
python3 touchdeck_companion_linux.py
```

Useful options:

```sh
python3 touchdeck_companion_linux.py --minimized
python3 touchdeck_companion_linux.py --autostart enable
python3 touchdeck_companion_linux.py --autostart disable
```

When a native webview backend is unavailable, the interface opens in the
default browser. The preferred local address is `http://127.0.0.1:8750/`.

## Sensors

Install psutil and the system sensor tools listed in `requirements.txt`.
Hardware availability depends on the kernel drivers and `lm-sensors` support
for the machine.

## Configuration

Files are stored below `~/.config/TouchDeck/`, or below
`$XDG_CONFIG_HOME/TouchDeck/` when that environment variable is set. The
main file is `companion_config.json`.

Autostart uses the user service `touchdeck-companion.service`.

The companion controls the sensor stream. Screen layout, per-slot labels,
styles, colors, brightness, clock, network, and firmware remain in the device
interface.
