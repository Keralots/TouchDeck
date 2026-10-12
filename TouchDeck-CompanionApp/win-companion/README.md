# TouchDeck Companion for Windows

The Windows companion selects PC sensors, sends them to TouchDeck over
UDP, and remains available from the system tray. Its configuration window uses
WebView2 through pywebview.

## Run from source

Python 3.10 or newer is recommended.

```bat
python -m pip install -r requirements.txt
python touchdeck_companion.py
```

Useful options:

```bat
python touchdeck_companion.py --minimized
python touchdeck_companion.py --autostart enable
python touchdeck_companion.py --autostart disable
```

When pywebview is unavailable, the companion opens its local interface in the
default browser. The preferred local address is `http://127.0.0.1:8750/`; an
available fallback port is selected automatically when 8750 is busy.

## LibreHardwareMonitor

CPU, RAM, and disk readings use psutil. Temperatures, fans, GPU readings, and
power normally require LibreHardwareMonitor:

1. Run LibreHardwareMonitor as Administrator.
2. Enable `Options -> Remote Web Server -> Run`.
3. Open Sensors in the companion and select Rescan sensors.

The Auto source mode prefers the REST API and falls back to WMI.

## Configuration and logs

The packaged application stores its files in `%APPDATA%\TouchDeck\`:

- `companion_config.json`
- `companion.log`

Source mode stores these files next to `touchdeck_companion.py`. This path is
separate from the SmallOLED companion, so both applications can coexist.

Autostart uses the per-user registry value
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\TouchDeckCompanion`.
It does not require administrator rights.

## Build the executable

```bat
python -m pip install pyinstaller
build_exe.bat
```

The output is `dist\TouchDeckCompanion.exe`. WebView2 is included with
Windows 11 and can be installed separately on Windows 10.

## Configuration ownership

The companion controls the sensor stream. Use the device interface for screen
layout, per-slot labels, display style, colors, brightness, clock, network, and
firmware updates. The Device page opens that interface and shows a live capture
from the real renderer.
