# TouchDeck Companion 4.1

The desktop companion discovers PC hardware sensors and sends selected values to
TouchDeck over UDP. Its web-style interface runs in a native window and
stays available from the system tray.

## Responsibilities

- The companion owns device targeting, sensor discovery, sensor selection,
  optional stream aliases, update frequency, autostart, logs, and backups.
- The TouchDeck device portal owns screen slots, per-slot labels, display
  styles, colors, brightness, clock settings, touch input, WiFi, and firmware.

The Device page reads the firmware's `/api/status` endpoint and displays the
real `/screen.bmp` capture. The Wallpapers page downloads Windows Spotlight
pictures or converts a folder of photos (read only) and uploads them to the deck. The retired 128x64 OLED emulator and layout-push
pipeline are not part of this companion.

## Telling look-alike sensors apart

A PC reports many sensors with the same name: one "Temperature" per drive, one
"Network Utilization" per interface. Every discovered sensor therefore carries
the device it belongs to plus a short detail line:

- Drives show their letters and physical disk index, so two identical models
  stay distinguishable (`C: - disk 2`, `disk 0`, `disk 1`).
- Network interfaces show their IPv4 address and link speed, and a badge for
  their link state. `active` marks the interface carrying the default route,
  which is the one the sensor list highlights with an accent border.

Drive letters come from `Win32_DiskDrive` and interface state from `psutil`;
both are refreshed on every sensor scan. The filter box searches the device
name and detail line too, so `wi-fi`, `C:`, or `active` narrow the list.

## Folders

| Folder | Purpose |
| --- | --- |
| `win-companion/` | Windows sensor backend, tray integration, autostart, and PyInstaller build. |
| `linux-companion/` | Linux sensor backend and systemd user autostart. |
| `companion-common/` | Shared state, configuration migration, local HTTP server, tests, and web UI. |

## Protocol

The companion sends UDP JSON to port 4210 using wire version 2.2. Each metric
contains an integer `id`, `name`, `value`, and `unit`. Persistent sensor IDs keep
device slot bindings stable when sensors are reordered, deselected, or
temporarily unavailable.

See the platform-specific README files for setup and packaging instructions.
