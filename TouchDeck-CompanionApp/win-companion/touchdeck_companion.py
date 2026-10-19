"""TouchDeck Companion for Windows."""

import psutil
import ipaddress
import socket
import time
import json
import os
import sys
import argparse
import threading
from datetime import datetime
from urllib import request as urllib_request
from urllib import error as urllib_error
import re

# Try to import pystray for system tray support
try:
    import pystray
    from PIL import Image, ImageDraw
    TRAY_AVAILABLE = True
except ImportError:
    TRAY_AVAILABLE = False

# Try to import pythoncom for COM initialization (needed for WMI with pythonw.exe)
try:
    import pythoncom
    PYTHONCOM_AVAILABLE = True
except ImportError:
    PYTHONCOM_AVAILABLE = False

# ---------------------------------------------------------------------------
# Frozen-build awareness (PyInstaller .exe) vs. plain-script execution.
#
# When packaged with PyInstaller --onefile, __file__ points at a temporary
# _MEIxxxx extraction folder that Windows deletes when the process exits, and
# a --windowed build has sys.stdout/stderr/stdin == None. Both of those break
# the original script (config saved into a vanishing temp dir; print()/input()
# crashing). We detect frozen mode here and adjust paths + I/O accordingly.
# ---------------------------------------------------------------------------
IS_FROZEN = getattr(sys, "frozen", False)

if IS_FROZEN:
    # The real .exe path - NOT the _MEIxxxx temp dir that __file__ resolves to.
    EXE_PATH = sys.executable
    APP_DIR = os.path.dirname(sys.executable)
else:
    EXE_PATH = os.path.abspath(__file__)
    APP_DIR = os.path.dirname(os.path.abspath(__file__))

# Shared, OS-neutral modules and web assets live in ../companion-common in
# script mode.
# A frozen build bundles them flat into _MEIPASS, so the import works without
# this path entry (and ../companion-common won't exist there -> skipped).
_COMMON_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "companion-common")
if os.path.isdir(_COMMON_DIR) and _COMMON_DIR not in sys.path:
    sys.path.insert(0, _COMMON_DIR)


def get_data_dir():
    """
    Per-user writable directory for the config file and log.

    Frozen .exe -> %APPDATA%\\TouchDeck so the config survives the .exe
    living in a read-only spot (Program Files) and survives the .exe being
    moved. Plain script -> next to the .py, matching the original behavior.
    """
    if IS_FROZEN:
        base = os.environ.get("APPDATA") or APP_DIR
        data_dir = os.path.join(base, "TouchDeck")
    else:
        data_dir = APP_DIR
    try:
        os.makedirs(data_dir, exist_ok=True)
    except Exception:
        data_dir = APP_DIR
    return data_dir


DATA_DIR = get_data_dir()
CONFIG_FILE = os.path.join(DATA_DIR, "companion_config.json")
LOG_FILE = os.path.join(DATA_DIR, "companion.log")


def _setup_frozen_io():
    """
    A --windowed PyInstaller build has no console: sys.stdout/stderr are None
    and this script's many print() calls would crash it. Redirect them to a log
    file (handy for debugging autostart), and give stdin a dummy read handle so
    a stray input() raises EOFError cleanly instead of AttributeError.
    """
    if not IS_FROZEN:
        return
    try:
        log = open(LOG_FILE, "a", encoding="utf-8", buffering=1)
        if sys.stdout is None:
            sys.stdout = log
        if sys.stderr is None:
            sys.stderr = log
    except Exception:
        try:
            devnull = open(os.devnull, "w")
            if sys.stdout is None:
                sys.stdout = devnull
            if sys.stderr is None:
                sys.stderr = devnull
        except Exception:
            pass
    if sys.stdin is None:
        try:
            sys.stdin = open(os.devnull, "r")
        except Exception:
            pass


_setup_frozen_io()


# Many status prints use Unicode glyphs (checkmarks, arrows). On a legacy cp1252
# console these would raise UnicodeEncodeError - harmless in v3 (console-only),
# but in v4 the same prints run on HTTP-server request threads, so an encode
# error would 500 a /save or /api/select. Make stdout/stderr lossy instead.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except Exception:
        pass


# Set True to make pause() never block (ensure_discovered: a web UI rescan must
# not stop on a console "Press Enter" prompt).
_SUPPRESS_PAUSE = False


def pause(message="Press Enter to continue..."):
    """input() that no-ops in a frozen / no-console build instead of crashing."""
    if IS_FROZEN or _SUPPRESS_PAUSE or not sys.stdin or not sys.stdin.isatty():
        return
    try:
        input(message)
    except (EOFError, RuntimeError):
        pass


def _splash_close():
    """Dismiss the PyInstaller startup splash screen, if this is a --splash build.

    The .exe is --onefile, so double-clicking it spends a few seconds extracting
    and discovering sensors before any window appears - long enough that users
    think nothing happened and double-click again. PyInstaller's splash shows a
    "Reading sensors, please wait..." box immediately (during extraction, before
    our Python even runs); we close it here the moment the real UI (config window
    or tray icon) is ready. Safe no-op for plain-script runs, where pyi_splash
    does not exist.
    """
    try:
        import pyi_splash  # injected by PyInstaller only in a --splash build
        if pyi_splash.is_alive():
            pyi_splash.close()
    except Exception:
        pass

# Default configuration
DEFAULT_CONFIG = {
    "version": "4.1",
    "esp32_ip": "",
    "udp_port": 4210,
    "update_interval": 3,
    "sensor_source": "auto",
    "metrics": []
}

# Global sensor database
sensor_database = {
    "system": [],    # psutil-based metrics (CPU%, RAM%, Disk%)
    "temperature": [],
    "fan": [],
    "load": [],
    "clock": [],
    "power": [],
    "data": [],       # Network/disk data (uploaded/downloaded GB)
    "throughput": [], # Network throughput (upload/download speed KB/s, MB/s)
    "gpu": [],        # GPU metrics (usage, memory, freq, fan, power) from LHM
    "other": []
}

# Byte counter and monotonic timestamp per (interface, direction).
_network_baselines = {}
_network_lock = threading.Lock()

# Global variable for discovered WMI namespace (can be auto-detected)
discovered_wmi_namespace = "root\\LibreHardwareMonitor"  # Default

# Global variables for REST API (alternative to WMI for LHM 0.9.5+)
rest_api_host = "localhost"
rest_api_port = 8085
use_rest_api = False  # Resolved from the sensor_source preference; see resolve_source()

# This core can read sensors two ways, so the UI may offer the choice. The Linux
# core reads sensors directly (no LHM) and sets this False, which is how the
# shared web UI knows to hide the control.
SUPPORTS_SOURCE_SELECT = True
SENSOR_SOURCES = ("auto", "rest", "wmi")

# Serialises source resolution + discovery: the HTTP server is threaded, so two
# requests can otherwise interleave a multi-step rescan.
_source_lock = threading.RLock()


class Snapshot(dict):
    """A sensor snapshot that remembers which source produced it.

    REST yields display strings ("53.0 C") and WMI yields floats, so a reader
    must know which it holds. Carrying that on the snapshot means a source switch
    mid-cycle can't cause the values to be misread.
    """
    def __init__(self, data, is_rest):
        super().__init__(data)
        self.is_rest = is_rest


class LHMHealthMonitor:
    """
    Monitors LibreHardwareMonitor REST API health.
    Tracks consecutive failures and provides exponential backoff for recovery.
    """
    def __init__(self):
        self.consecutive_failures = 0
        self.last_success_time = time.time()
        self.is_healthy = True
        self.last_warning_time = 0

    def record_success(self):
        """Record a successful API call"""
        if not self.is_healthy:
            print("  ✓ LHM connection restored!")
        self.consecutive_failures = 0
        self.last_success_time = time.time()
        self.is_healthy = True

    def record_failure(self):
        """Record a failed API call"""
        self.consecutive_failures += 1
        if self.consecutive_failures >= 2:  # Trigger faster (was 3)
            if self.is_healthy:
                print("  ⚠ LHM REST API unhealthy - entering recovery mode")
            self.is_healthy = False

    def get_retry_delay(self):
        """Exponential backoff: 3s, 6s, 12s, max 30s"""
        if self.consecutive_failures <= 1:
            return 3
        delay = min(3 * (2 ** (self.consecutive_failures - 1)), 30)
        return delay

    def should_print_warning(self):
        """Limit warning messages to once per 30 seconds"""
        now = time.time()
        if now - self.last_warning_time >= 30:
            self.last_warning_time = now
            return True
        return False


# Global health monitor instance
lhm_health_monitor = LHMHealthMonitor()


def is_lhm_process_running():
    """Check if LibreHardwareMonitor process is running"""
    lhm_names = ["librehardwaremonitor", "libre hardware monitor"]
    for proc in psutil.process_iter(['name']):
        try:
            proc_name = proc.info['name'].lower()
            if any(name in proc_name for name in lhm_names):
                return True
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return False


def discover_wmi_namespaces():
    """
    Quick check for WMI namespace (simplified for 0.9.5+ compatibility)
    Returns: (list_of_namespaces, working_namespace)
    """
    print("\nChecking WMI namespace...")

    namespace = "root\\LibreHardwareMonitor"
    try:
        import wmi
        w = wmi.WMI(namespace=namespace)
        sensors = list(w.Sensor())

        if len(sensors) > 0:
            print(f"  ✓ WMI working with {len(sensors)} sensors")
            return [namespace], namespace
        else:
            print(f"  ⚠ WMI accessible but 0 sensors (LibreHardwareMonitor 0.9.5+ issue)")
            print(f"  → Will use REST API fallback")
            return [], None
    except Exception as e:
        print(f"  ✗ WMI not accessible: {str(e)[:60]}")
        print(f"  → Will use REST API fallback")
        return [], None


def get_librehardwaremonitor_version():
    """
    Try to detect LibreHardwareMonitor version
    Returns: version string or None
    """
    version = None

    # Method 1: Check executable version
    try:
        import win32api
        import win32con
        import os

        # Common installation paths
        possible_paths = [
            r"C:\Program Files\LibreHardwareMonitor\LibreHardwareMonitor.exe",
            r"C:\Program Files (x86)\LibreHardwareMonitor\LibreHardwareMonitor.exe",
            r"C:\Users\Public\Desktop\LibreHardwareMonitor.exe",
        ]

        # Also check PATH
        try:
            import shutil
            exe_path = shutil.which("LibreHardwareMonitor.exe")
            if exe_path:
                possible_paths.insert(0, exe_path)
        except:
            pass

        for path in possible_paths:
            if os.path.exists(path):
                try:
                    info = win32api.GetFileVersionInfo(path, "\\")
                    ms = info['FileVersionMS']
                    ls = info['FileVersionLS']
                    version = f"{win32api.HIWORD(ms)}.{win32api.LOWORD(ms)}.{win32api.HIWORD(ls)}.{win32api.LOWORD(ls)}"
                    return version
                except:
                    pass
    except:
        pass

    # Method 2: Try to get version from WMI
    try:
        import wmi
        w = wmi.WMI()
        # Try to find LibreHardwareMonitor process and get its version
        import psutil
        for proc in psutil.process_iter(['name', 'exe']):
            try:
                if 'librehardwaremonitor' in proc.info['name'].lower():
                    exe_path = proc.info['exe']
                    if exe_path and os.path.exists(exe_path):
                        import win32api
                        info = win32api.GetFileVersionInfo(exe_path, "\\")
                        ms = info['FileVersionMS']
                        ls = info['FileVersionLS']
                        version = f"{win32api.HIWORD(ms)}.{win32api.LOWORD(ms)}.{win32api.HIWORD(ls)}.{win32api.LOWORD(ls)}"
                        return version
            except:
                pass
    except:
        pass

    return None


# ---------------------------------------------------------------------------
# Hardware context
#
# LibreHardwareMonitor happily reports seven sensors named "Temperature" and
# three named "Network Utilization". The only thing that tells them apart is the
# hardware they belong to, so discovery attaches a hardware name plus a short
# detail line (drive letters, IP address, link state) to every sensor.
# ---------------------------------------------------------------------------

_hardware_context = {"nics": {}, "disks": {}, "primary_ip": ""}
_DISK_KINDS = ("nvme", "hdd", "ssd", "storage")


def _primary_ipv4():
    """Return the local address the default route would use. Sends no traffic."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(0.4)
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except Exception:
        return ""
    finally:
        try:
            sock.close()
        except Exception:
            pass


def _link_speed_text(speed_mbps):
    """Format a psutil link speed (Mbit/s) for the sensor list."""
    if speed_mbps >= 1000:
        gigabit = speed_mbps / 1000.0
        text = ("%.1f" % gigabit).rstrip("0").rstrip(".")
        return "%s Gb/s" % text
    return "%d Mb/s" % speed_mbps


def scan_network_interfaces():
    """Describe every local interface so identical NIC sensors can be told apart.

    Interface names here match the hardware names LibreHardwareMonitor uses for
    its network devices ("Wi-Fi", "vEthernet (Default Switch)", ...).
    """
    primary_ip = _primary_ipv4()
    result = {}
    try:
        stats = psutil.net_if_stats()
        addresses = psutil.net_if_addrs()
    except Exception as exc:
        print("  (interface lookup unavailable: %s)" % exc)
        return result, primary_ip

    for name, stat in stats.items():
        ipv4 = ""
        for address in addresses.get(name, []):
            # 169.254.x.x is a link-local fallback address, not a working link.
            if address.family == socket.AF_INET and not address.address.startswith("169.254."):
                ipv4 = address.address
                break
        up = bool(stat.isup)
        if primary_ip and ipv4 == primary_ip:
            state = "primary"
        elif up and ipv4:
            state = "up"
        elif up:
            state = "idle"
        else:
            state = "down"
        result[name.strip().lower()] = {
            "name": name,
            "state": state,
            "ipv4": ipv4,
            "speed": int(stat.speed or 0),
        }
    return result, primary_ip


def scan_disk_letters():
    """Map physical disk index to drive letters so identical drives stay distinct.

    LibreHardwareMonitor numbers storage devices with the Windows physical disk
    index (/nvme/2 is disk 2), which is the same index Win32_DiskDrive reports.
    """
    letters = {}
    try:
        import gc
        import wmi as wmi_module
        client = wmi_module.WMI()
        for drive in client.Win32_DiskDrive():
            try:
                index = int(drive.Index)
            except (TypeError, ValueError):
                continue
            found = []
            for partition in drive.associators("Win32_DiskDriveToDiskPartition"):
                for logical in partition.associators("Win32_LogicalDiskToPartition"):
                    if logical.DeviceID:
                        found.append(logical.DeviceID)
            if found:
                letters[index] = sorted(set(found))
        # The WMI proxies form reference cycles. Collect them here so they are
        # released while COM is still initialized on this thread.
        del client
        gc.collect()
    except Exception as exc:
        print("  (drive letter lookup unavailable: %s)" % exc)
    return letters


def refresh_hardware_context():
    """Refresh the interface and drive-letter tables used during discovery."""
    nics, primary_ip = scan_network_interfaces()
    _hardware_context["nics"] = nics
    _hardware_context["primary_ip"] = primary_ip
    _hardware_context["disks"] = scan_disk_letters()
    return _hardware_context


def describe_hardware(sensor_id, parent_hardware):
    """Return (hardware, detail, state) for one sensor identifier.

    hardware is the device name, detail is a short disambiguator such as
    "C: - disk 2" or "192.168.0.10 - 2.5 Gb/s", and state is the link state of a
    network interface ("primary", "up", "idle", "down", "unknown") or "".
    """
    parts = [part for part in (sensor_id or "").split("/") if part]
    kind = parts[0].lower() if parts else ""
    index = parts[1] if len(parts) > 1 else ""
    hardware = (parent_hardware or "").strip()
    detail = ""
    state = ""

    if kind == "nic":
        info = _hardware_context["nics"].get(hardware.lower())
        if info:
            state = info["state"]
            pieces = [info["ipv4"] or "no address"]
            if info["speed"] > 0:
                pieces.append(_link_speed_text(info["speed"]))
            detail = " - ".join(pieces)
        else:
            state = "unknown"
            detail = "not reported by Windows"
    elif kind in _DISK_KINDS:
        try:
            drive_letters = _hardware_context["disks"].get(int(index), [])
        except (TypeError, ValueError):
            drive_letters = []
        pieces = []
        if drive_letters:
            pieces.append(" ".join(drive_letters))
        if index:
            pieces.append("disk %s" % index)
        detail = " - ".join(pieces)

    if not hardware:
        hardware = kind.upper() if kind else ""
    return hardware, detail, state


def _wmi_hardware_name(sensor_id, hardware_names):
    """Resolve a sensor identifier to its device using the longest matching prefix."""
    identifier = (sensor_id or "").lower()
    best_key = ""
    for key in hardware_names:
        if identifier.startswith(key + "/") and len(key) > len(best_key):
            best_key = key
    return hardware_names.get(best_key, "")


def _direction_qualified_name(sensor_name, sensor_type, index):
    """Name an unlabelled NIC counter, where index 0 and 1 are opposite directions."""
    lowered = sensor_name.lower()
    if any(word in lowered for word in ("upload", "download", "rx", "tx")):
        return sensor_name
    directions = {"0": "Download", "1": "Upload"} if sensor_type == "data" else {"0": "Upload", "1": "Download"}
    direction = directions.get(index)
    return f"{sensor_name} - {direction}" if direction else f"{sensor_name} #{index}"


def build_display_name(sensor_name, hardware, sensor_id):
    """Qualify a bare sensor name with the device it belongs to."""
    if hardware and hardware.lower() not in sensor_name.lower():
        return "%s [%s]" % (sensor_name, hardware)
    if hardware:
        return sensor_name
    parts = (sensor_id or "").split("/")
    if len(parts) > 1 and parts[1] and parts[1].lower() not in sensor_name.lower():
        return "%s [%s]" % (sensor_name, parts[1])
    return sensor_name


def extract_sensors_from_tree(node, sensor_list=None, parent_hardware=None, depth=0):
    """
    Recursively extract all sensors from LibreHardwareMonitor REST API tree structure.
    The API returns a hierarchical tree where actual sensors have a 'SensorId' field.
    Tracks parent hardware name to provide better context for sensors.

    The tree is Sensor > computer > hardware > sensor-type group > sensor, and
    devices such as a super-I/O chip add another hardware level. Only the group
    node directly above the sensors must be skipped, otherwise every reading ends
    up labelled "[Load]" or "[Temperatures]" instead of its actual device.
    """
    if sensor_list is None:
        sensor_list = []

    # If this node has a SensorId, it's an actual sensor
    if "SensorId" in node:
        # Add parent hardware name to sensor for better identification
        sensor_copy = node.copy()
        if parent_hardware:
            sensor_copy["_parent_hardware"] = parent_hardware
        sensor_list.append(sensor_copy)
        return sensor_list

    children = node.get("Children")
    children = children if isinstance(children, list) else []

    # A node whose children carry a SensorId is a sensor-type group ("Load",
    # "Temperatures"), never a device. Depth 0 is the tree root and depth 1 is the
    # computer name, so real hardware starts at depth 2.
    is_group = any("SensorId" in child for child in children)
    text = node.get("Text") or ""
    current_hardware = parent_hardware
    if depth >= 2 and text and not is_group:
        current_hardware = text

    for child in children:
        extract_sensors_from_tree(child, sensor_list, current_hardware, depth + 1)

    return sensor_list


def check_rest_api_connectivity(host, port):
    """
    Check if LibreHardwareMonitor REST API is accessible
    Returns: (success, sensor_count, error_message)
    """
    url = f"http://{host}:{port}/data.json"

    try:
        req = urllib_request.Request(url, method='GET')
        req.add_header('User-Agent', 'PC-Stats-Monitor/3.0')

        with urllib_request.urlopen(req, timeout=3) as response:
            if response.status == 200:
                data = response.read().decode('utf-8')
                root = json.loads(data)

                # Extract sensors from tree structure
                sensors = extract_sensors_from_tree(root)

                if len(sensors) > 0:
                    return True, len(sensors), None
                else:
                    return False, 0, "REST API returned no sensors"
            else:
                return False, 0, f"HTTP {response.status}"

    except urllib_error.HTTPError as e:
        return False, 0, f"HTTP error {e.code}"
    except urllib_error.URLError as e:
        return False, 0, f"Connection failed: {e.reason}"
    except json.JSONDecodeError as e:
        return False, 0, f"Invalid JSON response: {e}"
    except Exception as e:
        return False, 0, f"Unexpected error: {e}"


def discover_sensors_via_http(host, port):
    """
    Discover sensors via LibreHardwareMonitor REST API
    This is used when WMI fails (LHM 0.9.5+)
    """
    global sensor_database

    url = f"http://{host}:{port}/data.json"

    try:
        req = urllib_request.Request(url, method='GET')
        req.add_header('User-Agent', 'PC-Stats-Monitor/3.0')

        with urllib_request.urlopen(req, timeout=5) as response:
            if response.status != 200:
                print(f"  ✗ HTTP error {response.status}")
                return False

            data = response.read().decode('utf-8')
            root = json.loads(data)

            # Extract sensors from tree structure
            sensors = extract_sensors_from_tree(root)

            # Reset name tracker to ensure fresh unique names
            reset_generated_names()

            sensor_count = 0
            for sensor in sensors:
                # Map REST API fields to our sensor_database format
                sensor_id = sensor.get("SensorId", "")
                sensor_name = sensor.get("Text", "Unknown")
                sensor_type = sensor.get("Type", "").lower()
                sensor_value = sensor.get("Value", "0")

                # Skip if missing critical fields
                if not sensor_id or not sensor_name:
                    continue

                # Parse value from string (e.g., "45.0 °C" -> 45.0)
                original_value_str = str(sensor_value)
                try:
                    # Extract numeric value from string like "45.0 °C" or "12.1 %"
                    value_match = re.search(r'[-+]?\d*\.?\d+', original_value_str)
                    if value_match:
                        sensor_value = float(value_match.group())
                    else:
                        sensor_value = 0

                    # Normalize throughput to KB/s for ESP32
                    if sensor_type == "throughput":
                        value_upper = original_value_str.upper()
                        if "GB/S" in value_upper:
                            # GB/s → KB/s: multiply by 1024*1024
                            sensor_value = sensor_value * 1024 * 1024
                        elif "MB/S" in value_upper:
                            # MB/s → KB/s: multiply by 1024
                            sensor_value = sensor_value * 1024
                        elif "KB/S" in value_upper:
                            # Already KB/s, no conversion needed
                            pass
                        elif "B/S" in value_upper or not any(x in value_upper for x in ['/', 'S']):
                            # B/s or raw bytes → KB/s: divide by 1024
                            sensor_value = sensor_value / 1024
                        # Multiply by 10 to preserve 1 decimal place (ESP32 will divide by 10)
                        sensor_value = sensor_value * 10
                except:
                    sensor_value = 0

                # Determine unit based on type
                unit_map = {
                    "temperature": "C",  # Keep the wire unit ASCII and compact.
                    "fan": "RPM",
                    "load": "%",
                    "clock": "MHz",
                    "power": "W",
                    "voltage": "V",
                    "data": "GB",
                    "smalldata": "MB",
                    "control": "%",
                    "level": "%",
                    "throughput": "KB/s",
                }
                sensor_unit = unit_map.get(sensor_type, "")

                # Generate short name from sensor_id and sensor_name for uniqueness
                short_name = generate_short_name_from_id(sensor_id, sensor_type, sensor_name)

                # Build display name with device context
                parent_hardware = sensor.get("_parent_hardware", "")
                hardware, hardware_detail, nic_state = describe_hardware(sensor_id, parent_hardware)
                display_name = build_display_name(sensor_name, hardware, sensor_id)

                # The interface carrying the default route is the one the user
                # almost always means by "my network card".
                is_active_nic = nic_state == "primary"

                # Reclassify ambiguous types based on device context
                # Memory metrics are tagged as "data" but should be in "system"
                device_id_lower = sensor_id.lower()
                sensor_name_lower = sensor_name.lower()

                if sensor_type in ("data", "smalldata"):
                    # Check if this is memory-related (not network data)
                    if ("memory" in device_id_lower or "ram" in device_id_lower or
                        "vram" in device_id_lower or
                        ("gpu" in device_id_lower and ("memory" in sensor_name_lower or "vram" in sensor_name_lower))):
                        # Reclassify memory as system metric
                        sensor_type = "memory"

                sensor_info = {
                    "name": short_name,
                    "display_name": display_name,
                    "source": "wmi",  # Keep as "wmi" for compatibility
                    "type": sensor_type,
                    "unit": sensor_unit,
                    "wmi_identifier": sensor_id,
                    "wmi_sensor_name": sensor_name,
                    "custom_label": "",
                    "current_value": int(sensor_value),
                    "is_active_nic": is_active_nic,  # True for the default-route interface
                    "parent_hardware": parent_hardware,  # Raw tree name
                    "hardware": hardware,  # Device this reading belongs to
                    "hardware_detail": hardware_detail,  # Drive letters, IP address, link speed
                    "nic_state": nic_state  # primary / up / idle / down / unknown
                }

                # GPU sensors go to the dedicated "gpu" category.
                if _is_gpu_sensor(sensor_id):
                    sensor_database["gpu"].append(sensor_info)
                elif sensor_type == "temperature":
                    sensor_database["temperature"].append(sensor_info)
                elif sensor_type == "fan":
                    sensor_database["fan"].append(sensor_info)
                elif sensor_type == "load":
                    sensor_database["load"].append(sensor_info)
                elif sensor_type == "clock":
                    sensor_database["clock"].append(sensor_info)
                elif sensor_type == "power":
                    sensor_database["power"].append(sensor_info)
                elif sensor_type == "memory":  # Reclassified memory metrics
                    sensor_database["system"].append(sensor_info)
                elif sensor_type in ("data", "smalldata"):  # Now only actual network data
                    sensor_database["data"].append(sensor_info)
                elif sensor_type == "throughput":
                    sensor_database["throughput"].append(sensor_info)
                else:
                    sensor_database["other"].append(sensor_info)

                sensor_count += 1

            if sensor_count > 0:
                print(f"  ✓ Found {sensor_count} hardware sensors via REST API:")
                print(f"    - GPU:         {len(sensor_database['gpu'])}")
                print(f"    - Temperatures: {len(sensor_database['temperature'])}")
                print(f"    - Fans: {len(sensor_database['fan'])}")
                print(f"    - Loads: {len(sensor_database['load'])}")
                print(f"    - Clocks: {len(sensor_database['clock'])}")
                print(f"    - Power: {len(sensor_database['power'])}")
                print(f"    - Data: {len(sensor_database['data'])}")
                print(f"    - Throughput: {len(sensor_database['throughput'])}")
                if len(sensor_database['other']) > 0:
                    print(f"    - Other: {len(sensor_database['other'])}")
                return True
            else:
                print("  ⚠ REST API returned 0 sensors")
                return False

    except urllib_error.HTTPError as e:
        print(f"  ✗ HTTP error {e.code}")
        return False
    except urllib_error.URLError as e:
        print(f"  ✗ Connection failed: {e.reason}")
        return False
    except Exception as e:
        print(f"  ✗ Error: {e}")
        return False


# Global tracker for generated names to ensure uniqueness
_generated_names = set()


def reset_generated_names():
    """Reset the name tracker - call before sensor discovery"""
    global _generated_names
    _generated_names = set()


def _make_unique_name(base_name):
    """Ensure name is unique by adding suffix if needed"""
    global _generated_names

    # Truncate base to max 10 chars
    base_name = base_name[:10]

    if base_name not in _generated_names:
        _generated_names.add(base_name)
        return base_name

    # Add numeric suffix to make unique
    for i in range(1, 100):
        candidate = f"{base_name[:8]}{i}" if len(base_name) > 8 else f"{base_name}{i}"
        candidate = candidate[:10]
        if candidate not in _generated_names:
            _generated_names.add(candidate)
            return candidate

    return base_name  # Fallback


def _extract_context_suffix(sensor_name):
    """Extract context suffix from sensor name"""
    name_lower = sensor_name.lower()

    # Memory/data context
    if "used" in name_lower:
        return "_U"
    elif "available" in name_lower or "avail" in name_lower:
        return "_A"
    elif "capacity" in name_lower:
        return "_CAP"
    elif "total" in name_lower:
        return "_TOT"
    elif "free" in name_lower:
        return "_F"

    # Temperature context
    elif "core max" in name_lower:
        return "_MAX"
    elif "core avg" in name_lower or "average" in name_lower:
        return "_AVG"
    elif "hotspot" in name_lower:
        return "_HOT"
    elif "junction" in name_lower:
        return "_JNC"

    return ""


def generate_short_name_from_id(sensor_id, sensor_type, sensor_name=""):
    """
    Generate unique short name from sensor_id and sensor_name (REST API format)
    Uses sensor_name context to differentiate similar sensors
    """
    parts = sensor_id.split('/')
    name_lower = sensor_name.lower()

    # Get context suffix from sensor name
    context = _extract_context_suffix(sensor_name)

    if len(parts) >= 4:
        device = parts[1]  # e.g., "intelcpu", "gpu-nvidia", "lpc", "nic"
        device_lower = device.lower()
        device_idx = parts[2] if len(parts) > 2 else "0"
        sensor_idx = parts[-1]  # Last part is usually the sensor index

        # CPU sensors
        if "cpu" in device_lower:
            if sensor_type == "load":
                if "total" in name_lower or sensor_idx == "0":
                    base = "CPU"
                else:
                    base = f"CPU_C{sensor_idx}"
            elif sensor_type == "temperature":
                if "core max" in name_lower:
                    base = "CPU_MAX"
                elif "core avg" in name_lower or "average" in name_lower:
                    base = "CPU_AVG"
                elif "core" in name_lower and "p-core" not in name_lower and "e-core" not in name_lower:
                    base = f"CPUT{sensor_idx}"
                elif "p-core" in name_lower:
                    base = f"CPUP{sensor_idx}"
                elif "e-core" in name_lower:
                    base = f"CPUE{sensor_idx}"
                elif "ccd" in name_lower:
                    base = f"CCD{sensor_idx}T"
                else:
                    base = f"CPUT{sensor_idx}" if sensor_idx != "0" else "CPUT"
            elif sensor_type == "power":
                if "package" in name_lower:
                    base = "CPU_PKG"
                elif "core" in name_lower:
                    base = "CPU_COR"
                else:
                    base = f"CPUW{sensor_idx}" if sensor_idx != "0" else "CPUW"
            elif sensor_type == "clock":
                base = f"CPUCLK{sensor_idx}" if sensor_idx != "0" else "CPUCLK"
            else:
                base = f"CPU_{sensor_idx}"
            return _make_unique_name(base)

        # GPU sensors
        elif "gpu" in device_lower or "nvidia" in device_lower or "amd" in device_lower:
            gpu_idx = "" if device_idx == "0" else device_idx
            if sensor_type == "load":
                if "memory" in name_lower or "vram" in name_lower:
                    base = f"VRAM{gpu_idx}"
                elif "core" in name_lower or sensor_idx == "0":
                    base = f"GPU{gpu_idx}"
                else:
                    base = f"GPU{gpu_idx}_{sensor_idx}"
            elif sensor_type == "temperature":
                if "hotspot" in name_lower:
                    base = f"GPU{gpu_idx}_HOT"
                elif "memory" in name_lower or "vram" in name_lower:
                    base = f"VRAM{gpu_idx}T"
                else:
                    base = f"GPUT{gpu_idx}"
            elif sensor_type == "power":
                base = f"GPUW{gpu_idx}"
            elif sensor_type == "clock":
                if "memory" in name_lower:
                    base = f"VCLK{gpu_idx}"
                else:
                    base = f"GCLK{gpu_idx}"
            elif sensor_type == "fan":
                base = f"GPUF{gpu_idx}_{sensor_idx}" if sensor_idx != "0" else f"GPUF{gpu_idx}"
            elif sensor_type in ("data", "smalldata"):
                # GPU memory data
                base = f"VRAM{gpu_idx}{context}"
            else:
                base = f"GPU{gpu_idx}_{sensor_idx}"
            return _make_unique_name(base)

        # LPC/Motherboard sensors (VRM, PCH, System temps, etc.)
        elif "lpc" in device_lower or "motherboard" in device_lower or "mainboard" in device_lower:
            if sensor_type == "temperature":
                if "vrm" in name_lower:
                    base = "VRM_T"
                elif "mos" in name_lower:
                    base = "MOS_T"
                elif "pch" in name_lower:
                    base = "PCH_T"
                elif "cpu" in name_lower and "socket" in name_lower:
                    base = "CPUS_T"
                elif "system" in name_lower:
                    base = f"SYS{sensor_idx}T"
                elif "pcie" in name_lower or "pci" in name_lower:
                    base = f"PCIE{sensor_idx}T"
                elif "m.2" in name_lower or "m2" in name_lower:
                    base = f"M2_{sensor_idx}T"
                elif "chipset" in name_lower:
                    base = "CHIP_T"
                else:
                    # Generic LPC temperature
                    base = f"MB{sensor_idx}T"
            elif sensor_type == "fan":
                if "cpu" in name_lower:
                    base = "CPUF"
                elif "pump" in name_lower:
                    base = "PUMP"
                elif "chassis" in name_lower:
                    base = f"CHS{sensor_idx}F"
                elif "system" in name_lower:
                    # Extract fan number from name like "System Fan #1"
                    fan_match = re.search(r'#(\d+)', sensor_name)
                    if fan_match:
                        base = f"SYS{fan_match.group(1)}F"
                    else:
                        base = f"SYS{sensor_idx}F"
                elif "aux" in name_lower:
                    base = f"AUX{sensor_idx}F"
                else:
                    base = f"FAN{sensor_idx}"
            elif sensor_type == "voltage":
                if "vcore" in name_lower or "cpu" in name_lower:
                    base = "VCORE"
                elif "vram" in name_lower or "memory" in name_lower:
                    base = "VMEM"
                elif "+12v" in name_lower or "12v" in name_lower:
                    base = "V12"
                elif "+5v" in name_lower or "5v" in name_lower:
                    base = "V5"
                elif "+3.3v" in name_lower or "3.3v" in name_lower:
                    base = "V3_3"
                else:
                    base = f"V{sensor_idx}"
            elif sensor_type == "control":
                base = f"CTL{sensor_idx}"
            else:
                base = f"LPC{sensor_idx}"
            return _make_unique_name(base)

        # Memory/RAM sensors
        elif "memory" in device_lower or "ram" in device_lower:
            if sensor_type in ("data", "smalldata", "memory"):
                if "vram" in name_lower:
                    base = f"VRAM{context}"
                elif "capacity" in name_lower:
                    base = f"RAM_CAP"
                elif "used" in name_lower:
                    base = "RAM_U"
                elif "available" in name_lower or "avail" in name_lower:
                    base = "RAM_A"
                else:
                    base = f"RAM{context}" if context else f"RAM{sensor_idx}"
            elif sensor_type == "load":
                base = "RAM"
            else:
                base = f"RAM{sensor_idx}"
            return _make_unique_name(base)

        # Network sensors
        elif "nic" in device_lower or "network" in device_lower:
            net_idx = "" if device_idx == "0" else device_idx
            if sensor_type == "throughput":
                if "upload" in name_lower or "sent" in name_lower:
                    base = f"NET{net_idx}_U"
                elif "download" in name_lower or "received" in name_lower:
                    base = f"NET{net_idx}_D"
                else:
                    # Use sensor index: 0=upload, 1=download typically
                    if sensor_idx == "0":
                        base = f"NET{net_idx}_U"
                    else:
                        base = f"NET{net_idx}_D"
            elif sensor_type == "data":
                if "upload" in name_lower or "sent" in name_lower:
                    base = f"NTD{net_idx}_U"
                elif "download" in name_lower or "received" in name_lower:
                    base = f"NTD{net_idx}_D"
                else:
                    base = f"NTD{net_idx}_{sensor_idx}"
            else:
                base = f"NET{net_idx}_{sensor_idx}"
            return _make_unique_name(base)

        # Storage (HDD/SSD/NVMe)
        elif "hdd" in device_lower or "ssd" in device_lower or "nvme" in device_lower:
            drv_idx = "" if device_idx == "0" else device_idx
            if "hdd" in device_lower:
                prefix = f"HDD{drv_idx}"
            elif "ssd" in device_lower:
                prefix = f"SSD{drv_idx}"
            else:
                prefix = f"NVM{drv_idx}"

            if sensor_type == "temperature":
                base = f"{prefix}T"
            elif sensor_type == "load":
                base = f"{prefix}%"
            elif sensor_type == "data":
                base = f"{prefix}D"
            else:
                base = f"{prefix}_{sensor_idx}"
            return _make_unique_name(base)

    # Fallback: Create descriptive name from sensor_name + sensor_id
    if sensor_name:
        # Use first word of sensor name + type abbreviation
        words = sensor_name.replace("-", " ").replace("_", " ").split()
        if words:
            base = words[0][:4].upper()
            type_suffix = {"temperature": "T", "fan": "F", "load": "%",
                          "power": "W", "voltage": "V", "clock": "C"}.get(sensor_type, "")
            base = f"{base}{type_suffix}"
            return _make_unique_name(base)

    # Last resort fallback
    if len(parts) >= 2:
        device = parts[1].replace("-", "")[:4].upper()
        return _make_unique_name(f"{device}{sensor_idx}")


def _is_gpu_sensor(identifier):
    """
    Return True if the LHM sensor identifier belongs to a GPU device.
    Uses the same device-part logic as generate_short_name_from_id so that
    AMD CPU sensors (amdcpu) are NOT mistaken for GPU sensors.
    """
    parts = identifier.lower().split('/')
    if len(parts) < 2:
        return False
    device = parts[1]
    # "cpu" check must come first to exclude intelcpu / amdcpu
    if "cpu" in device:
        return False
    return "gpu" in device or "nvidia" in device or "amd" in device


def check_wmi_connectivity(allow_rest_fallback=True):
    """
    Diagnostics: Check if LibreHardwareMonitor WMI namespace is accessible
    Returns: (success, error_message, suggestion)

    allow_rest_fallback=False keeps this from switching to REST when WMI looks
    broken, so an explicit "wmi" preference stays honoured (the auto preference
    is what opts into falling back).
    """
    print("\n" + "-" * 60)
    print("DIAGNOSTICS: Checking LibreHardwareMonitor connectivity...")
    print("-" * 60)

    # Check 1: Verify required modules are installed
    print("\n[Check 1/4] Verifying required Python modules...")
    try:
        import wmi
        print("  ✓ pywin32 and wmi modules are installed")
    except ImportError as e:
        missing = str(e).split("'")[1] if "'" in str(e) else "pywin32/wmi"
        return False, f"Missing required module: {missing}", (
            "FIX: Install required modules:\n"
            "   pip install pywin32 wmi\n\n"
            "   Or run: python -m pip install pywin32 wmi"
        )

    # Check 2: Verify LibreHardwareMonitor process is running
    print("\n[Check 2/4] Checking if LibreHardwareMonitor is running...")
    try:
        import psutil
        found_lhm = False
        lhm_names = ["librehardwaremonitor", "libre hardware monitor",
                     "hwmonitor", "hardware monitor"]
        for proc in psutil.process_iter(['name']):
            try:
                proc_name = proc.info['name'].lower()
                if any(name in proc_name for name in lhm_names):
                    found_lhm = True
                    print(f"  ✓ Found LibreHardwareMonitor process: {proc.info['name']}")
                    break
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass

        # Try to detect version if process is running
        if found_lhm:
            version = get_librehardwaremonitor_version()
            if version:
                print(f"  → Detected LibreHardwareMonitor version: {version}")
                # Check for known problematic versions
                version_parts = version.split('.')
                if len(version_parts) >= 2:
                    major = int(version_parts[0])
                    minor = int(version_parts[1])
                    # Version 0.9.5+ has known WMI issues
                    if major == 0 and minor >= 9 and len(version_parts) >= 3:
                        patch = int(version_parts[2])
                        if patch >= 5:
                            print("\n  ⚠⚠⚠ WARNING: LibreHardwareMonitor 0.9.5+ has BROKEN WMI support! ⚠⚠⚠")
                            print("  → Version 0.9.5 introduced a bug that breaks WMI sensor reporting")
                            print("  → See: https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/issues/2088")
                            print("  → RECOMMENDED: Downgrade to 0.9.4 from GitHub Releases page")
            else:
                print(f"  → Could not detect version (please report this)")

        if not found_lhm:
            return False, "LibreHardwareMonitor is not running", (
                "FIX: Start LibreHardwareMonitor first\n\n"
                "   1. Launch LibreHardwareMonitor\n"
                "   2. IMPORTANT: Right-click and select 'Run as Administrator'\n"
                "   3. Keep it running while using this script"
            )
    except Exception as e:
        print(f"  ⚠ Could not check processes: {e}")

    # Check 3: Auto-discover WMI namespace
    print("\n[Check 3/4] Auto-discovering WMI namespace...")
    global discovered_wmi_namespace

    # Run auto-discovery
    _found_namespaces, working_namespace = discover_wmi_namespaces()

    if working_namespace:
        # Update global namespace with the one that works
        discovered_wmi_namespace = working_namespace
        print(f"\n  → Using namespace: {working_namespace}")
    else:
        # Auto-discovery failed - likely LibreHardwareMonitor 0.9.5+ with broken WMI
        # Try REST API fallback before giving up (LHM 0.9.5+ workaround)
        print(f"\n  ⚠ No working WMI namespace found (LHM 0.9.5+ issue)")

        global use_rest_api
        if not allow_rest_fallback:
            return False, "No working WMI namespace found", (
                "WMI is unavailable and the sensor source is pinned to WMI.\n\n"
                "Set the source back to Auto to use the REST API instead."
            )

        print(f"  → Trying REST API fallback...")
        print(f"  → Checking http://{rest_api_host}:{rest_api_port}/data.json")

        rest_success, rest_count, rest_error = check_rest_api_connectivity(rest_api_host, rest_api_port)

        # Debug: print REST API check result
        if rest_error:
            print(f"  ✗ REST API failed: {rest_error}")

        if rest_success and rest_count > 0:
            # REST API works! Use it instead of WMI
            use_rest_api = True
            print(f"\n  ✓✓✓ REST API WORKS! Using REST API instead of WMI ✓✓✓")
            print(f"  → Found {rest_count} sensors via REST API")
            print(f"  → This bypasses the WMI bug in LibreHardwareMonitor 0.9.5+")
            print(f"  → Make sure 'Remote Web Server' is enabled in LibreHardwareMonitor")
            print(f"     (Options → Remote Web Server → Run)")
            # Skip remaining checks - REST API is working
            return True, None, None

        # REST API also failed - provide simplified troubleshooting
        return False, "No working WMI namespace found", (
            "FIX: LibreHardwareMonitor 0.9.5+ has broken WMI support.\n\n"
            "SOLUTION (3 steps):\n\n"
            "1. Enable REST API in LibreHardwareMonitor:\n"
            "   Options → Remote Web Server → Run (port 8085)\n\n"
            "2. If that doesn't work, downgrade to 0.9.4:\n"
            "   https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases\n\n"
            "3. If still failing, add Windows Defender exclusion:\n"
            "   Add exclusion for LibreHardwareMonitor folder in Windows Security"
        )

    # Check 4: Verify we can actually read sensor data
    print("\n[Check 4/4] Testing sensor data access...")
    try:
        import wmi
        w = wmi.WMI(namespace=discovered_wmi_namespace)
        sensors = list(w.Sensor())

        if len(sensors) == 0:
            # CRITICAL: Namespace exists but no sensors found
            # Try REST API fallback before giving up (LHM 0.9.5+ workaround)
            if not allow_rest_fallback:
                return False, "WMI namespace accessible but contains 0 sensors!", (
                    "WMI exposes no sensors and the sensor source is pinned to WMI.\n\n"
                    "Set the source back to Auto to use the REST API instead."
                )

            print(f"  ⚠ WMI returned 0 sensors - trying REST API fallback...")
            print(f"  → Checking http://{rest_api_host}:{rest_api_port}/data.json")

            rest_success, rest_count, rest_error = check_rest_api_connectivity(rest_api_host, rest_api_port)

            if rest_success and rest_count > 0:
                # REST API works! Use it instead of WMI
                use_rest_api = True
                print(f"\n  ✓✓✓ REST API WORKS! Using REST API instead of WMI ✓✓✓")
                print(f"  → Found {rest_count} sensors via REST API")
                print(f"  → This bypasses the WMI bug in LibreHardwareMonitor 0.9.5+")
                print(f"  → Make sure 'Remote Web Server' is enabled in LibreHardwareMonitor")
                print(f"     (Options → Remote Web Server → Run)")
                return True, None, None

            # REST API also failed - show comprehensive error with all options
            error_msg = "WMI namespace accessible but contains 0 sensors!"
            if rest_error:
                error_msg += f"\nREST API also failed: {rest_error}"

            return False, error_msg, (
                "FIX: LibreHardwareMonitor driver is not providing sensor data.\n\n"
                "SOLUTION (3 steps):\n\n"
                "1. Enable REST API in LibreHardwareMonitor:\n"
                "   Options → Remote Web Server → Run (port 8085)\n\n"
                "2. If that doesn't work, downgrade to 0.9.4:\n"
                "   https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases\n\n"
                "3. If still failing, check for conflicts:\n"
                "   Close HWiNFO64, HWMonitor, AIDA64, or add Windows Defender exclusion"
            )

        print(f"  ✓ Sensor data is readable ({len(sensors)} sensors found)")
    except Exception as e:
        return False, f"Cannot read sensor data: {e}", (
            "FIX: Sensor access error\n\n"
            "   This may be a permission issue.\n"
            "   Try running both LibreHardwareMonitor and this script as Administrator."
        )

    print("\n" + "-" * 60)
    print("✓ All diagnostics passed!")
    print("-" * 60)
    return True, None, None


def print_troubleshooting_header(error_title):
    """Print a formatted troubleshooting header"""
    print("\n" + "!" * 60)
    print(f"ERROR: {error_title}")
    print("!" * 60)
    print("\nTROUBLESHOOTING STEPS:")
    print("-" * 60)


def print_troubleshooting_footer():
    """Print a formatted troubleshooting footer"""
    print("-" * 60)
    print("\nIf the problem persists, please share a screenshot of this")
    print("entire error message when reporting the issue.")
    print("\n" + "!" * 60 + "\n")


def discover_network_sensors():
    """Return psutil throughput sensors for active, non-loopback interfaces."""
    try:
        stats = psutil.net_if_stats()
        addresses = psutil.net_if_addrs()
    except OSError as exc:
        print("  (network sensor discovery unavailable: %s)" % exc)
        return []

    sensors = []
    for nic, stat in stats.items():
        if not stat.isup:
            continue
        flags = getattr(stat, "flags", "").split(",")
        if "loopback" in flags or "loopback" in nic.lower() or nic.lower() in ("lo", "lo0"):
            continue
        ip_addresses = [address.address for address in addresses.get(nic, [])
                        if address.family in (socket.AF_INET, socket.AF_INET6)]
        if any(ipaddress.ip_address(address).is_loopback for address in ip_addresses):
            continue
        for direction, title, name in (("recv", "Download", "NET_D"),
                                       ("sent", "Upload", "NET_U")):
            sensors.append({
                "name": name,
                "display_name": f"{title} Speed [{nic}] (psutil)",
                "source": "psutil",
                "type": "throughput",
                "unit": "KB/s",
                "psutil_method": f"net_{direction}:{nic}",
                "custom_label": "",
                "current_value": 0
            })
    return sensors


def discover_sensors():
    """
    Discover all available sensors from LibreHardwareMonitor and psutil
    """
    print("=" * 60)
    print("PC STATS MONITOR v3.0 - SENSOR DISCOVERY")
    print("=" * 60)

    # Add psutil system metrics
    print("\n[1/2] Discovering system metrics (psutil)...")

    # Warm up psutil for accurate readings
    psutil.cpu_percent(interval=0.1)
    sensor_database["system"].append({
        "name": "CPU",
        "display_name": "CPU Usage",
        "source": "psutil",
        "type": "percent",
        "unit": "%",
        "psutil_method": "cpu_percent",
        "custom_label": "",
        "current_value": int(psutil.cpu_percent(interval=0))
    })

    sensor_database["system"].append({
        "name": "RAM",
        "display_name": "RAM Usage",
        "source": "psutil",
        "type": "percent",
        "unit": "%",
        "psutil_method": "virtual_memory.percent",
        "custom_label": "",
        "current_value": int(psutil.virtual_memory().percent)
    })

    sensor_database["system"].append({
        "name": "RAM_GB",
        "display_name": "RAM Used (GB)",
        "source": "psutil",
        "type": "memory",
        "unit": "GB",
        "psutil_method": "virtual_memory.used",
        "custom_label": "",
        "current_value": int(psutil.virtual_memory().used / (1024**3))
    })

    sensor_database["system"].append({
        "name": "DISK",
        "display_name": "Disk C: Usage",
        "source": "psutil",
        "type": "percent",
        "unit": "%",
        "psutil_method": "disk_usage",
        "custom_label": "",
        "current_value": int(psutil.disk_usage('C:\\').percent)
    })

    sensor_database["throughput"].extend(discover_network_sensors())

    print(f"  Found {len(sensor_database['system'])} system metrics")

    # Drive letters and interface link state are what make look-alike sensors
    # ("Temperature", "Network Utilization") identifiable in the sensor list.
    refresh_hardware_context()

    # Discover LibreHardwareMonitor sensors
    print("\n[2/2] Discovering hardware sensors (LibreHardwareMonitor)...")

    # The source preference is already resolved (see ensure_discovered), so only
    # probe WMI when that's where we actually landed. check_wmi_connectivity can
    # still flip us to REST from here, which is the LHM 0.9.5 broken-WMI path.
    if not use_rest_api:
        success, error_msg, suggestion = check_wmi_connectivity(
            allow_rest_fallback=source_preference() != "wmi")

        if not success:
            print_troubleshooting_header(error_msg)
            print(suggestion)
            print_troubleshooting_footer()
            print("\n⚠ WARNING: Hardware sensors will NOT be available.")
            print("  Only system metrics (CPU, RAM, Disk) can be monitored.")
            print("\n  Press Enter to continue with system metrics only...")
            pause()
            return

    # Check if we should use REST API instead of WMI (LHM 0.9.5+ workaround)
    if use_rest_api:
        print("\n→ Using REST API for sensor discovery (LibreHardwareMonitor 0.9.5+ mode)")
        if not discover_sensors_via_http(rest_api_host, rest_api_port):
            print("\n⚠ REST API discovery failed. No hardware sensors available.")
            print("  Only system metrics (CPU, RAM, Disk) can be monitored.")
        print("\n" + "=" * 60)
        return

    # If diagnostics passed and not using REST API, proceed with WMI sensor discovery
    try:
        import wmi
        # Use the auto-discovered namespace
        w = wmi.WMI(namespace=discovered_wmi_namespace)
        sensors = w.Sensor()

        sensor_count = 0
        # Reset name tracker to ensure fresh unique names
        reset_generated_names()

        # The Hardware class carries the friendly device names ("Samsung SSD 980
        # PRO 1TB", "Wi-Fi") that the Sensor class only hints at through its
        # identifier prefix.
        hardware_names = {}
        try:
            for device in w.Hardware():
                identifier = (device.Identifier or "").rstrip("/").lower()
                if identifier and device.Name:
                    hardware_names[identifier] = device.Name
        except Exception as exc:
            print(f"  (hardware name lookup unavailable: {exc})")

        for sensor in sensors:
            # Generate short name for ESP32 display (using same function as REST API)
            short_name = generate_short_name_from_id(sensor.Identifier, sensor.SensorType.lower(), sensor.Name)

            # Enhance display name with device context for the GUI
            parent_hardware = _wmi_hardware_name(sensor.Identifier, hardware_names)
            hardware, hardware_detail, nic_state = describe_hardware(sensor.Identifier, parent_hardware)
            sensor_type_lower = sensor.SensorType.lower()

            # Some NIC counters are named only "Data" or "Throughput"; their
            # identifier index is the only hint at the direction.
            qualified_name = sensor.Name
            identifier_parts = sensor.Identifier.split('/')
            if len(identifier_parts) >= 4 and sensor_type_lower in ("data", "throughput"):
                device_info = identifier_parts[1].lower()
                if 'nic' in device_info or 'network' in device_info:
                    qualified_name = _direction_qualified_name(
                        sensor.Name, sensor_type_lower, identifier_parts[-1])
            display_name = build_display_name(qualified_name, hardware, sensor.Identifier)

            # Get current sensor value
            try:
                current_value = int(sensor.Value) if sensor.Value else 0
            except:
                current_value = 0

            sensor_info = {
                "name": short_name,
                "display_name": display_name,
                "source": "wmi",
                "type": sensor_type_lower,
                "unit": get_unit_from_type(sensor.SensorType),
                "wmi_identifier": sensor.Identifier,
                "wmi_sensor_name": qualified_name,
                "custom_label": "",
                "current_value": current_value,
                "is_active_nic": nic_state == "primary",
                "parent_hardware": parent_hardware,
                "hardware": hardware,
                "hardware_detail": hardware_detail,
                "nic_state": nic_state
            }

            # GPU sensors go to the dedicated "gpu" category.
            if _is_gpu_sensor(sensor.Identifier):
                sensor_database["gpu"].append(sensor_info)
                sensor_count += 1
            elif sensor_type_lower == "temperature":
                sensor_database["temperature"].append(sensor_info)
                sensor_count += 1
            elif sensor_type_lower == "fan":
                sensor_database["fan"].append(sensor_info)
                sensor_count += 1
            elif sensor_type_lower == "load":
                sensor_database["load"].append(sensor_info)
                sensor_count += 1
            elif sensor_type_lower == "clock":
                sensor_database["clock"].append(sensor_info)
                sensor_count += 1
            elif sensor_type_lower == "power":
                sensor_database["power"].append(sensor_info)
                sensor_count += 1
            elif sensor_type_lower == "data":
                sensor_database["data"].append(sensor_info)
                sensor_count += 1
            elif sensor_type_lower == "throughput":
                sensor_database["throughput"].append(sensor_info)
                sensor_count += 1
            else:
                sensor_database["other"].append(sensor_info)
                sensor_count += 1

        print(f"  Found {sensor_count} hardware sensors:")
        print(f"    - GPU:          {len(sensor_database['gpu'])}")
        print(f"    - Temperatures: {len(sensor_database['temperature'])}")
        print(f"    - Fans: {len(sensor_database['fan'])}")
        print(f"    - Loads: {len(sensor_database['load'])}")
        print(f"    - Clocks: {len(sensor_database['clock'])}")
        print(f"    - Power: {len(sensor_database['power'])}")
        print(f"    - Data: {len(sensor_database['data'])}")
        print(f"    - Throughput: {len(sensor_database['throughput'])}")
        if len(sensor_database['other']) > 0:
            print(f"    - Other: {len(sensor_database['other'])}")

    except ImportError:
        print("  WARNING: pywin32/wmi not installed. Hardware sensors unavailable.")
        print("  Install with: pip install pywin32 wmi")
    except Exception as e:
        # Fallback error handling (should rarely trigger since diagnostics run first)
        print_troubleshooting_header(f"Unexpected error during sensor discovery: {e}")
        print("This error occurred after initial diagnostics passed.")
        print("\nPossible causes:")
        print("  - LibreHardwareMonitor was closed after diagnostics")
        print("  - WMI connection was lost during sensor enumeration")
        print("  - System resource limitation")
        print("\nPlease try again:")
        print("  1. Make sure LibreHardwareMonitor is running as Administrator")
        print("  2. Restart this script")
        print_troubleshooting_footer()

    print("\n" + "=" * 60)
    print("\nℹ NOTE: Sensor values in GUI are static (captured at launch time)")
    print("  This helps you identify active sensors and their typical readings.")


def get_unit_from_type(sensor_type):
    """
    Map sensor type to display unit
    """
    unit_map = {
        "Temperature": "C",
        "Load": "%",
        "Fan": "RPM",
        "Clock": "MHz",
        "Power": "W",
        "Voltage": "V",
        "Data": "GB",
        "Throughput": "KB/s"  # Network throughput speeds
    }
    return unit_map.get(sensor_type, "")


def load_config():
    """
    Load configuration from file with version checking
    """
    if not os.path.exists(CONFIG_FILE):
        return None

    try:
        # utf-8-sig: a BOM (an editor, a PowerShell redirect) is otherwise a
        # parse error that would quarantine a perfectly good config.
        with open(CONFIG_FILE, 'r', encoding='utf-8-sig') as f:
            config = json.load(f)
        if not isinstance(config, dict):
            raise ValueError("top level is %s, not an object"
                             % type(config).__name__)

        # Version check - force reconfiguration for old versions
        config_version = config.get("version", "1.0")
        if config_version < "2.1":
            print("\n" + "=" * 60)
            print("  CONFIGURATION UPDATE REQUIRED")
            print("=" * 60)
            print(f"\n⚠ Configuration format updated (v{config_version} → v2.1)")
            print("\nMajor bug fixes in this version:")
            print("  ✓ Fixed duplicate metric names (HDDT, RAMUSED, etc.)")
            print("  ✓ Fixed memory metrics appearing in wrong category")
            print("  ✓ Removed companion markers (^^) from display")
            print("  ✓ Custom labels now visible in preview")
            print("  ✓ Improved GUI layout")
            print("\n→ You will need to reconfigure your metrics in the GUI.")

            # Backup old config
            backup_path = CONFIG_FILE.replace(".json", f"_v{config_version}_backup.json")
            try:
                import shutil
                shutil.copy(CONFIG_FILE, backup_path)
                print(f"\n  Old config backed up to: {backup_path}")
            except Exception as e:
                print(f"\n  Warning: Could not backup config: {e}")

            # Delete old config to force reconfiguration
            try:
                os.remove(CONFIG_FILE)
                print(f"  Old config removed: {CONFIG_FILE}")
            except Exception as e:
                print(f"  Warning: Could not remove old config: {e}")

            print("\n" + "=" * 60 + "\n")
            pause("Press Enter to continue to configuration GUI...")
            return None

        print(f"\n✓ Loaded configuration from {CONFIG_FILE}")
        print(f"  Selected metrics: {len(config.get('metrics', []))}")
        return config
    except Exception as e:
        # Keep a copy before defaults take over: the next save would otherwise
        # overwrite the damaged file and the whole setup would be unrecoverable.
        try:
            from config_model import quarantine_unreadable_config
            quarantine_unreadable_config(CONFIG_FILE, e)
        except Exception:
            print(f"\n✗ Error loading config: {e}")
        return None


def save_config(config):
    """
    Save configuration to file
    """
    try:
        with open(CONFIG_FILE, 'w') as f:
            json.dump(config, f, indent=2)
        print(f"\n✓ Configuration saved to {CONFIG_FILE}")
        return True
    except Exception as e:
        print(f"\n✗ Error saving config: {e}")
        return False


# Windows "run at login" registry location (per-user, no admin needed).
AUTOSTART_REG_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
AUTOSTART_VALUE_NAME = "TouchDeckCompanion"


def _autostart_command():
    """
    The exact command Windows runs at login.

    Frozen .exe -> just the .exe (it already runs windowed/no-console).
    Plain script -> pythonw.exe + the script path so no console window appears.
    The 10s startup delay gives LibreHardwareMonitor time to come up first.
    """
    if IS_FROZEN:
        return f'"{EXE_PATH}" --minimized --startup-delay 10'
    pythonw = sys.executable.replace("python.exe", "pythonw.exe")
    return f'"{pythonw}" "{EXE_PATH}" --minimized --startup-delay 10'


def is_autostart_enabled():
    """Return True if our HKCU Run value exists."""
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0,
                            winreg.KEY_QUERY_VALUE) as key:
            winreg.QueryValueEx(key, AUTOSTART_VALUE_NAME)
            return True
    except FileNotFoundError:
        return False
    except Exception:
        return False


def setup_autostart(enable=True):
    """
    Enable/disable "run at Windows login" via the per-user HKCU Run key.

    This needs no admin rights and no extra packages (winreg is stdlib). The
    monitor reads sensors over LibreHardwareMonitor's REST API, so it does not
    need to be elevated itself - only LHM does (its own setting).
    """
    import winreg

    if enable:
        command = _autostart_command()
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, AUTOSTART_REG_KEY) as key:
            winreg.SetValueEx(key, AUTOSTART_VALUE_NAME, 0, winreg.REG_SZ, command)
        print(f"\n✓ Autostart enabled!")
        print(f"  Registry: HKCU\\{AUTOSTART_REG_KEY}\\{AUTOSTART_VALUE_NAME}")
        print(f"  Command:  {command}")
        print(f"  Startup delay: 10 seconds (waiting for LHM to start)")
        return True
    else:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0,
                                winreg.KEY_SET_VALUE) as key:
                winreg.DeleteValue(key, AUTOSTART_VALUE_NAME)
            print(f"\n✓ Autostart disabled!")
        except FileNotFoundError:
            print("\n✓ Autostart was not enabled (nothing to remove)")
        return True


# AutoConfigPreviewDialog class removed - will be revisited later


def _parse_rest_value(value_str, is_throughput):
    """
    Parse a LibreHardwareMonitor REST value string (e.g. "45.0 C", "12.3 MB/s")
    into the int the ESP32 expects. Throughput is normalized to KB/s and scaled
    by 10 to preserve one decimal place (the ESP32 divides by 10 when showing).
    Returns 0 if the string holds no number.
    """
    try:
        match = re.search(r'[-+]?\d*[.,]?\d+', value_str)
        if not match:
            return 0
        # LHM formats through the .NET current culture, so a de/pl/fr machine
        # emits "12,3 MB/s". It never groups thousands (en-US gives "2044 RPM",
        # not "2,044"), so a comma here is always the decimal point -- without
        # this the regex stopped at the comma and silently dropped the decimal.
        value = float(match.group().replace(',', '.'))
        if is_throughput:
            value_upper = value_str.upper()
            if "GB/S" in value_upper:
                value = value * 1024 * 1024          # GB/s -> KB/s
            elif "MB/S" in value_upper:
                value = value * 1024                 # MB/s -> KB/s
            elif "KB/S" in value_upper:
                pass                                 # already KB/s
            elif "B/S" in value_upper or not any(x in value_upper for x in ['/', 'S']):
                value = value / 1024                 # B/s or raw bytes -> KB/s
            value = value * 10
        return int(value)
    except Exception:
        return 0


def build_rest_snapshot(host, port):
    """
    Fetch /data.json ONCE and return {SensorId: raw_value_string}, or None on
    failure. Lets a whole update cycle resolve every REST metric from a single
    request instead of one HTTP GET + full-tree parse per metric.
    """
    url = f"http://{host}:{port}/data.json"
    try:
        req = urllib_request.Request(url, method='GET')
        req.add_header('User-Agent', 'PC-Stats-Monitor/3.0')
        with urllib_request.urlopen(req, timeout=1) as response:  # 1s for fast failure detection
            if response.status != 200:
                lhm_health_monitor.record_failure()
                return None
            data = response.read().decode('utf-8')
            root = json.loads(data)
            sensors = extract_sensors_from_tree(root)
            snapshot = {}
            for sensor in sensors:
                sid = sensor.get("SensorId", "")
                if sid:
                    snapshot[sid] = str(sensor.get("Value", "0"))
            lhm_health_monitor.record_success()
            return Snapshot(snapshot, is_rest=True)
    except Exception:
        lhm_health_monitor.record_failure()
        return None


# Cached WMI connection so we don't rebind COM (an expensive operation) on every
# metric of every cycle. Reset to None on error to force a clean reconnect.
# WMI/COM objects are apartment-threaded: a connection made on one thread can't
# be used from another. The monitor loop AND the layout editor's live-preview
# worker both build snapshots, so the connection is cached PER THREAD (each
# thread CoInitializes itself) instead of in one shared global.
_wmi_tls = threading.local()


def _wql_literal(s):
    """Escape a WQL single-quoted string literal (backslash is WQL's escape)."""
    return s.replace("\\", "\\\\").replace("'", "\\'")


def build_wmi_snapshot(identifiers=None):
    """
    Read LibreHardwareMonitor WMI sensors ONCE and return {Identifier: float},
    or None on failure. Caches a per-thread connection.

    identifiers: restrict the query to these sensor Identifiers (the configured
    metrics). None reads every sensor -- only wanted by callers that genuinely
    need the full set, e.g. sensor discovery.

    Uses late-bound COM (ExecQuery) rather than the `wmi` module's Sensor():
    that wrapper builds a full property-introspected object per instance, which
    costs ~1.3s of CPU for ~300 sensors -- more than one core's worth every cycle.
    Selecting only the two fields we read drops it to ~87ms, and filtering to the
    configured sensors to ~13ms (the provider evaluates the WHERE, so unselected
    sensors are never marshalled).
    """
    try:
        import win32com.client
        conn = getattr(_wmi_tls, "connection", None)
        if conn is None:
            conn = win32com.client.GetObject("winmgmts:root\\LibreHardwareMonitor")
            _wmi_tls.connection = conn
        query = "SELECT Identifier,Value FROM Sensor"
        if identifiers is not None:
            wanted = sorted(set(identifiers))
            if not wanted:
                return Snapshot({}, is_rest=False)
            query += " WHERE " + " OR ".join(
                "Identifier='%s'" % _wql_literal(i) for i in wanted)
        snapshot = {}
        for sensor in conn.ExecQuery(query):
            try:
                snapshot[sensor.Identifier] = float(sensor.Value)
            except Exception:
                pass
        return Snapshot(snapshot, is_rest=False)
    except Exception:
        _wmi_tls.connection = None  # Force reconnect next cycle (this thread)
        return None


def _network_throughput(nic, direction):
    """Read a byte-counter delta as integer tenths of KB/s."""
    key = (nic, direction)
    with _network_lock:
        try:
            # Raw counters let us detect resets instead of hiding them as wraps.
            counters = psutil.net_io_counters(pernic=True, nowrap=False)
        except OSError:
            return None
        counter = counters.get(nic)
        if counter is None:
            _network_baselines.pop(key, None)
            return 0
        now = time.monotonic()
        current = getattr(counter, "bytes_" + direction)
        previous = _network_baselines.get(key)
        _network_baselines[key] = (current, now)
        if previous is None:
            return 0
        delta = current - previous[0]
        elapsed = now - previous[1]
        if delta < 0 or elapsed <= 0:
            return 0
        return int(round(delta / elapsed / 1024 * 10))


def get_metric_value(metric_config, snapshot=None):
    """
    Get current value for a configured metric.

    For hardware (LHM) metrics, `snapshot` is the per-cycle map built once by
    send_metrics (REST: {SensorId: value_string}, WMI: {Identifier: float}), so
    the whole sensor source is fetched/parsed only once per cycle. A snapshot of
    None means the source was unavailable this cycle -> return None (stale) so
    the caller falls back to the last good value.

    Returns: int value on success, None on failure (for WMI/REST API sources)
    """
    source = metric_config["source"]

    if source == "psutil":
        method = metric_config["psutil_method"]

        if method == "cpu_percent":
            return int(psutil.cpu_percent(interval=0))
        elif method == "virtual_memory.percent":
            return int(psutil.virtual_memory().percent)
        elif method == "virtual_memory.used":
            return int(psutil.virtual_memory().used / (1024**3))  # GB
        elif method == "disk_usage":
            return int(psutil.disk_usage('C:\\').percent)
        elif method.startswith(("net_recv:", "net_sent:")):
            direction, nic = method.split(":", 1)
            return _network_throughput(nic, direction[4:])
        return None

    if source == "wmi":
        if snapshot is None:
            return None  # Source unavailable this cycle -> use cached value

        # Trust the snapshot's own provenance, not the live global: the source
        # can be switched from the web-UI thread between build_snapshot() picking
        # a source and this interpreting the result, which would otherwise parse
        # REST strings as WMI floats (or vice versa). Plain dicts (older callers,
        # tests) fall back to the global.
        if getattr(snapshot, "is_rest", use_rest_api):
            sensor_id = metric_config.get("wmi_identifier", "")
            if sensor_id and sensor_id in snapshot:
                is_throughput = metric_config.get("unit", "") == "KB/s"
                return _parse_rest_value(snapshot[sensor_id], is_throughput)
            # API responded but this sensor wasn't present
            return 0

        # WMI snapshot: {Identifier: float}
        identifier = metric_config.get("wmi_identifier", "")
        if identifier in snapshot:
            value = snapshot[identifier]
            # For throughput: WMI returns B/s, convert to KB/s and scale by 10
            if metric_config.get("unit", "") == "KB/s":
                value = value / 1024  # B/s -> KB/s
                value = value * 10    # Preserve 1 decimal place
            return int(value)
        return None  # Sensor not present

    return None


# Status codes (must match ESP32 config.h)
STATUS_OK = 1
STATUS_API_ERROR = 2
STATUS_LHM_NOT_RUNNING = 3
STATUS_LHM_STARTING = 4
STATUS_UNKNOWN_ERROR = 5


def source_preference(config=None):
    """The configured sensor_source, defaulting to auto.

    Configs written before this setting existed simply lack the key (load_config
    does not merge DEFAULT_CONFIG), so always read it defensively.
    """
    if config is None:
        config = load_config() or {}
    pref = str(config.get("sensor_source", "auto")).lower()
    return pref if pref in SENSOR_SOURCES else "auto"


def resolve_source(pref="auto"):
    """Point `use_rest_api` at the source `pref` asks for. Returns the resolved
    source name ("rest" / "wmi").

    auto -> REST when its server answers, else WMI. REST is preferred because it
            is cheaper (~0.26ms vs ~2.9ms CPU per sweep) and fresher (WMI lags
            LHM by about one refresh). WMI stays the fallback since LHM's Remote
            Web Server is off by default.
    rest / wmi -> forced, no fallback, so the choice is predictable. `auto` is
            there for anyone who wants the fallback.
    """
    global use_rest_api
    with _source_lock:
        if pref == "wmi":
            use_rest_api = False
        elif pref == "rest":
            use_rest_api = True
        else:
            ok, count, _ = check_rest_api_connectivity(rest_api_host, rest_api_port)
            use_rest_api = bool(ok and count > 0)
        return "rest" if use_rest_api else "wmi"


def build_snapshot(config, force=False):
    """Fetch the hardware-sensor source ONCE for this cycle (REST or WMI), or
    None when there are no hardware metrics or the source is unavailable. Shared
    by the UDP sender and the dialog's live preview so both read one source.

    force=True bypasses the is_healthy fast-path skip, so a caller without its own
    recovery probing (the live-preview dialog) keeps attempting and self-recovers
    instead of getting stuck once the health monitor trips."""
    if not any(m.get("source") == "wmi" for m in config["metrics"]):
        return None
    if use_rest_api:
        # Skip the request while the API is known down (fast path); the monitoring
        # loop handles periodic recovery probing separately. force overrides this.
        if force or lhm_health_monitor.is_healthy:
            return build_rest_snapshot(rest_api_host, rest_api_port)
        return None
    # Only the configured sensors are ever looked up (get_metric_value indexes by
    # wmi_identifier), so don't pay to marshal the rest.
    return build_wmi_snapshot([m["wmi_identifier"] for m in config["metrics"]
                               if m.get("source") == "wmi" and m.get("wmi_identifier")])


def collect_metrics(config, snapshot, last_good_values=None, status_code=STATUS_OK):
    """Collect values and assemble the UDP payload. PURE (no socket).

    Shared by the UDP sender and local live-value status. Carries the single
    status, timestamp, and last-good policy.

    Returns: (payload, values_by_id, has_fresh_data, last_good_values, stale_count)
    """
    if last_good_values is None:
        last_good_values = {}

    has_fresh_data = False
    stale_count = 0
    payload = {
        "version": "2.2",
        "status": status_code,  # LHM health status code
        "timestamp": "",        # Will be set based on data freshness
        "metrics": []
    }
    values_by_id = {}

    for metric_config in config["metrics"]:
        value = get_metric_value(metric_config, snapshot)
        metric_id = metric_config["id"]

        if value is not None:
            last_good_values[metric_id] = value  # fresh data -> update cache
            has_fresh_data = True
        else:
            value = last_good_values.get(metric_id, 0)  # stale -> use cached
            stale_count += 1
        values_by_id[metric_id] = value

        # Use custom label if set, otherwise the generated name
        display_name = metric_config.get("custom_label", "")
        if not display_name:
            display_name = metric_config["name"]
        payload["metrics"].append({
            "id": metric_id,
            "name": display_name,
            "value": value,
            "unit": metric_config["unit"]
        })

    # Override status if data is stale (catches the API failing before the
    # health monitor trips).
    total_metrics = len(config["metrics"])
    if total_metrics > 0 and stale_count >= total_metrics:
        if status_code == STATUS_OK:
            status_code = STATUS_API_ERROR
    elif stale_count > 0 and stale_count >= total_metrics * 0.5:
        if status_code == STATUS_OK:
            status_code = STATUS_API_ERROR
    payload["status"] = status_code

    # Empty timestamp signals the ESP32 that data may be stale. Only stamp a
    # time when data is fresh AND status is healthy: psutil keeps returning
    # fresh values while LHM is down, so the device's last-OK time would track now.
    payload["timestamp"] = (datetime.now().strftime('%H:%M')
                            if has_fresh_data and status_code == STATUS_OK else "")
    return payload, values_by_id, has_fresh_data, last_good_values, stale_count


def create_tray_icon():
    """Create a simple system tray icon"""
    if not TRAY_AVAILABLE:
        return None

    # The deck's splash mark: four rounded tiles, drawn large and scaled down
    # for smooth corners (same artwork as touchdeck.ico).
    def create_image():
        size, ss = 64, 8
        n = size * ss
        cell, gap = n * 4 / 9, n / 9
        image = Image.new('RGBA', (n, n), (0, 0, 0, 0))
        dc = ImageDraw.Draw(image)
        for i, color in enumerate(('#F6C56B', '#5CC9A7', '#B3A6FF', '#7DB5FF')):
            x, y = (i % 2) * (cell + gap), (i // 2) * (cell + gap)
            dc.rounded_rectangle([x, y, x + cell - 1, y + cell - 1], radius=cell * 0.3, fill=color)
        return image.resize((size, size), Image.LANCZOS)

    return create_image()


# ---------------------------------------------------------------------------
# Single-instance coordination
#
# The app normally lives in the system tray. Without a guard, double-clicking
# the .exe again (very common when the tray icon goes unnoticed) would spin up
# a SECOND monitor + tray icon, silently duplicating the UDP traffic. We use a
# fixed localhost TCP port as both a lock and a tiny IPC channel:
#   - The running monitor ("primary") binds the port and listens for commands.
#   - A later launch detects the primary via a handshake, asks it to show its
#     window, and exits.
# ---------------------------------------------------------------------------
SINGLE_INSTANCE_HOST = "127.0.0.1"
# Fixed localhost port: lock + IPC for the one running monitor. Kept below 49152
# (the dynamic range) so it avoids Windows' reserved WinNAT/Hyper-V port blocks,
# which can otherwise make bind() fail with WSAEACCES. If it is ever unavailable,
# acquire_single_instance() falls back to "standalone" and the app still runs.
SINGLE_INSTANCE_PORT = 42120
_SINGLE_INSTANCE_MAGIC = b"TDECK1"

# Held for the process lifetime so the OS keeps the lock; released on exit.
_single_instance_sock = None
# Set by the IPC listener when a later launch asks the primary to bring its
# (hidden-to-tray) window forward. app_window's show-watcher reacts to it.
_show_event = threading.Event()


def _detect_running_primary():
    """Return True if OUR already-running monitor answers on the port.

    Uses a handshake (not merely "is the port open") so an unrelated program
    that happens to hold the port is not mistaken for our instance.
    """
    try:
        with socket.create_connection((SINGLE_INSTANCE_HOST, SINGLE_INSTANCE_PORT),
                                      timeout=1) as conn:
            conn.sendall(b"ping")
            conn.settimeout(1)
            return conn.recv(32).startswith(_SINGLE_INSTANCE_MAGIC)
    except OSError:
        return False


def acquire_single_instance():
    """Decide this launch's role with respect to the single running monitor.

    Returns one of:
      "primary"    - no monitor was running; we grabbed the lock and should run
                     it (the listening socket is kept in _single_instance_sock).
      "secondary"  - our monitor is already running; this launch should hand off.
      "standalone" - the port is held by something that is not us; run unguarded
                     (degraded: no duplicate protection, but the app still works).
    """
    global _single_instance_sock

    if _detect_running_primary():
        return "secondary"

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind((SINGLE_INSTANCE_HOST, SINGLE_INSTANCE_PORT))
        sock.listen(5)
    except OSError:
        sock.close()
        return "standalone"  # port busy, but the handshake says it is not us

    _single_instance_sock = sock
    return "primary"


def start_single_instance_listener():
    """Serve handshake pings and 'show' requests from later launches."""
    server = _single_instance_sock
    if server is None:
        return

    def listener():
        while True:
            try:
                conn, _ = server.accept()
            except OSError:
                break  # socket closed on shutdown
            try:
                conn.settimeout(2)  # don't let a silent peer wedge the listener
                data = conn.recv(64) or b""
                if b"show" in data:
                    _show_event.set()
                conn.sendall(_SINGLE_INSTANCE_MAGIC + b" ok")
            except OSError:
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    threading.Thread(target=listener, daemon=True, name="single-instance").start()


def signal_primary_show():
    """Ask the already-running primary to bring its window forward. Best-effort."""
    try:
        with socket.create_connection((SINGLE_INSTANCE_HOST, SINGLE_INSTANCE_PORT),
                                      timeout=2) as conn:
            conn.sendall(b"show")
            conn.settimeout(2)
            try:
                conn.recv(32)
            except OSError:
                pass
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Platform hooks consumed by the shared web modules (server.py / app_window.py).
# The Linux core implements the same four; keeps the shared files OS-neutral.
# ---------------------------------------------------------------------------
def source_text():
    """Short description of the active hardware-sensor source (status readout)."""
    hw = sum(len(v) for k, v in sensor_database.items() if k != "system")
    if hw > 0:
        return ("REST API" if use_rest_api else "WMI") + " (%d sensors)" % hw
    if not is_lhm_process_running():
        return "psutil only (LHM off)"
    return "psutil only (no sensors)"


def source_banner():
    """{level,text} banner for the Sensors page."""
    hw = sum(len(v) for k, v in sensor_database.items() if k != "system")
    if hw > 0:
        src = "REST API" if use_rest_api else "WMI"
        return {"level": "ok", "text": "LibreHardwareMonitor connected via %s - %d hardware sensors available." % (src, hw)}
    if not is_lhm_process_running():
        return {"level": "err", "text": "LibreHardwareMonitor is not running. Only CPU / RAM / Disk are available. Start it (as Administrator), then Rescan."}
    return {"level": "warn", "text": "LibreHardwareMonitor is running but exposes no sensors. Enable Options - Remote Web Server - Run, then Rescan."}


def ensure_discovered(rescan=False):
    """Populate sensor_database lazily / on rescan (WMI needs COM on this thread)."""
    global _SUPPRESS_PAUSE
    with _source_lock:
        if any(sensor_database[k] for k in sensor_database) and not rescan:
            return
        if PYTHONCOM_AVAILABLE:
            try:
                pythoncom.CoInitialize()
            except Exception:
                pass
        try:
            for k in list(sensor_database.keys()):
                sensor_database[k] = []
            # Resolve the configured preference. This used to hard-reset to WMI,
            # which silently undid the REST detection detect_source() had already
            # done -- discovery runs later (the web UI hits /api/sensors on load),
            # so WMI always won and REST was unreachable while WMI worked.
            resolve_source(source_preference())
            _SUPPRESS_PAUSE = True
            try:
                discover_sensors()
            except Exception as e:
                print("Discovery error: %s" % e)
            finally:
                _SUPPRESS_PAUSE = False
        finally:
            if PYTHONCOM_AVAILABLE:
                try:
                    pythoncom.CoUninitialize()
                except Exception:
                    pass


def detect_source(state):
    """Resolve the configured sensor source for the monitor, then set the status
    text. Runs once at startup on a worker thread."""
    if PYTHONCOM_AVAILABLE:
        try:
            pythoncom.CoInitialize()
        except Exception:
            pass
    try:
        pref = source_preference()
        if resolve_source(pref) == "wmi":
            try:
                # May fall back to REST when WMI is broken (LHM 0.9.5+), unless
                # the user pinned WMI.
                check_wmi_connectivity(allow_rest_fallback=pref != "wmi")
            except Exception:
                pass
        state.set_source_text(source_text())
    finally:
        if PYTHONCOM_AVAILABLE:
            try:
                pythoncom.CoUninitialize()
            except Exception:
                pass


REST_REPROBE_S = 10
_last_rest_reprobe = 0.0


def recover_source(state, config, snapshot):
    """Auto mode stuck on WMI with no readings (LHM's web server was down at
    startup): re-probe REST and rediscover once it answers."""
    global _last_rest_reprobe
    if use_rest_api or snapshot or source_preference(config) != "auto":
        return
    if not any(m.get("source") == "wmi" for m in config.get("metrics") or []):
        return
    now = time.monotonic()
    if now - _last_rest_reprobe < REST_REPROBE_S:
        return
    _last_rest_reprobe = now
    ok, count, _ = check_rest_api_connectivity(rest_api_host, rest_api_port)
    if not (ok and count > 0):
        return
    print("  ✓ LHM REST API is up - switching from WMI and rescanning")
    ensure_discovered(rescan=True)
    state.set_source_text(source_text())


def main():
    """Main entry point (v4: pywebview web UI + system tray, single process)."""
    parser = argparse.ArgumentParser(description='TouchDeck Companion')
    parser.add_argument('--configure', action='store_true', help='Open the configuration window')
    parser.add_argument('--edit', action='store_true', help='Open the configuration window')
    parser.add_argument('--autostart', choices=['enable', 'disable'], help='Enable/disable autostart')
    parser.add_argument('--minimized', action='store_true', help='Start hidden in the system tray')
    parser.add_argument('--startup-delay', type=int, default=0,
                        help='Delay before starting (autostart waits for LibreHardwareMonitor)')
    args = parser.parse_args()

    if args.minimized:
        _splash_close()
    if args.startup_delay > 0:
        print(f"Waiting {args.startup_delay}s for system services to start...")
        time.sleep(args.startup_delay)

    # COM init for WMI on the main thread (worker threads CoInitialize themselves).
    if PYTHONCOM_AVAILABLE:
        try:
            pythoncom.CoInitialize()
        except Exception:
            pass

    if args.autostart:
        try:
            if setup_autostart(args.autostart == 'enable'):
                print("\nAutostart updated (runs minimized to the tray at login).")
        except Exception as e:
            print(f"\nError setting up autostart: {e}")
        _splash_close()
        return

    print("\n" + "=" * 60)
    print("  TOUCHDECK COMPANION - web UI + system tray")
    print("=" * 60 + "\n")

    # Single running instance: if our monitor is already up, bring its window
    # forward instead of spawning a duplicate tray icon / monitor.
    role = acquire_single_instance()
    if role == "secondary":
        print("TouchDeck Companion is already running - showing its window.")
        signal_primary_show()
        _splash_close()
        return
    if role == "primary":
        start_single_instance_listener()

    _splash_close()
    import app_window
    # Show the window on a normal launch; stay hidden in the tray only for an
    # autostart-at-login (--minimized) that did not also ask to configure.
    start_hidden = args.minimized and not (args.configure or args.edit)
    app_window.run(sys.modules[__name__], start_hidden=start_hidden,
                   notify_startup=args.minimized)


if __name__ == "__main__":
    main()
