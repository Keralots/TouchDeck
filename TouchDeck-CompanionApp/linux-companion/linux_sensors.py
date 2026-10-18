"""Linux sensor discovery and readout (psutil, hwmon, NVML, amdgpu) for the companion."""

import psutil
import socket
import time
import os
import sys
import glob as _glob

_COMMON_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "companion-common")
if os.path.isdir(_COMMON_DIR) and _COMMON_DIR not in sys.path:
    sys.path.insert(0, _COMMON_DIR)

# Global sensor database
sensor_database = {
    "system": [],      # psutil-based metrics (CPU%, RAM%, Disk%)
    "temperature": [], # Hardware temperatures
    "fan": [],         # Fan speeds
    "data": [],        # Network data (total uploaded/downloaded)
    "throughput": [],  # Network throughput (current upload/download speed)
    "gpu": [],         # GPU metrics (usage, memory, freq, fan, power)
    "other": []
}

# Network tracking for speed calculation
network_last_sample = {}
network_last_time = None

# NVML handles for NVIDIA GPU (populated during sensor discovery or lazily)
nvml_handles = {}


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
        text = ("%.1f" % (speed_mbps / 1000.0)).rstrip("0").rstrip(".")
        return "%s Gb/s" % text
    return "%d Mb/s" % speed_mbps


def scan_network_interfaces():
    """Describe every interface so identical network sensors can be told apart."""
    primary_ip = _primary_ipv4()
    result = {}
    try:
        stats = psutil.net_if_stats()
        addresses = psutil.net_if_addrs()
    except Exception as exc:
        print(f"  (interface lookup unavailable: {exc})")
        return result

    for name, stat in stats.items():
        ipv4 = ""
        for address in addresses.get(name, []):
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
        pieces = [ipv4 or "no address"]
        speed = int(stat.speed or 0)
        if speed > 0:
            pieces.append(_link_speed_text(speed))
        result[name] = {"state": state, "detail": " - ".join(pieces)}
    return result


def discover_sensors():
    """
    Discover all available sensors from psutil on Linux
    """
    print("=" * 60)
    print("PC STATS MONITOR v2.0 - LINUX EDITION - SENSOR DISCOVERY")
    print("=" * 60)

    # Add psutil system metrics
    print("\n[1/3] Discovering system metrics (psutil)...")

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
        "name": "SWAP",
        "display_name": "Swap Usage",
        "source": "psutil",
        "type": "percent",
        "unit": "%",
        "psutil_method": "swap_memory.percent",
        "custom_label": "",
        "current_value": int(psutil.swap_memory().percent)
    })

    sensor_database["system"].append({
        "name": "SWAP_GB",
        "display_name": "Swap Used (GB)",
        "source": "psutil",
        "type": "memory",
        "unit": "GB",
        "psutil_method": "swap_memory.used",
        "custom_label": "",
        "current_value": int(psutil.swap_memory().used / (1024**3))
    })

    sensor_database["system"].append({
        "name": "DISK",
        "display_name": "Disk / Usage",
        "source": "psutil",
        "type": "percent",
        "unit": "%",
        "psutil_method": "disk_usage",
        "custom_label": "",
        "current_value": int(psutil.disk_usage('/').percent)
    })

    print(f"  Found {len(sensor_database['system'])} system metrics")

    # Discover hardware sensors (temperatures)
    print("\n[2/3] Discovering hardware sensors (temperatures & fans)...")
    sensor_count = 0

    try:
        # Temperature sensors
        if hasattr(psutil, "sensors_temperatures"):
            temps = psutil.sensors_temperatures()

            for sensor_name, entries in temps.items():
                for entry in entries:
                    # Generate short name
                    short_name = generate_short_name_linux(entry.label, "temperature", sensor_name)

                    # Generate display name
                    display_name = f"{entry.label} [{sensor_name}]" if entry.label else f"Sensor [{sensor_name}]"

                    # Get current value
                    try:
                        current_value = int(entry.current) if entry.current else 0
                    except:
                        current_value = 0

                    sensor_info = {
                        "name": short_name,
                        "display_name": display_name,
                        "source": "psutil_temp",
                        "type": "temperature",
                        "unit": "C",
                        "sensor_key": sensor_name,
                        "sensor_label": entry.label,
                        "wmi_sensor_name": entry.label or "Temperature",
                        "hardware": sensor_name,
                        "custom_label": "",
                        "current_value": current_value
                    }

                    sensor_database["temperature"].append(sensor_info)
                    sensor_count += 1

        # Fan sensors
        if hasattr(psutil, "sensors_fans"):
            fans = psutil.sensors_fans()

            for sensor_name, entries in fans.items():
                for entry in entries:
                    # Generate short name
                    short_name = generate_short_name_linux(entry.label, "fan", sensor_name)

                    # Generate display name
                    display_name = f"{entry.label} [{sensor_name}]" if entry.label else f"Fan [{sensor_name}]"

                    # Get current value
                    try:
                        current_value = int(entry.current) if entry.current else 0
                    except:
                        current_value = 0

                    sensor_info = {
                        "name": short_name,
                        "display_name": display_name,
                        "source": "psutil_fan",
                        "type": "fan",
                        "unit": "RPM",
                        "sensor_key": sensor_name,
                        "sensor_label": entry.label,
                        "wmi_sensor_name": entry.label or "Fan",
                        "hardware": sensor_name,
                        "custom_label": "",
                        "current_value": current_value
                    }

                    sensor_database["fan"].append(sensor_info)
                    sensor_count += 1

        print(f"  Found {sensor_count} hardware sensors:")
        print(f"    - Temperatures: {len(sensor_database['temperature'])}")
        print(f"    - Fans: {len(sensor_database['fan'])}")

    except Exception as e:
        print(f"  WARNING: Could not access hardware sensors: {e}")

    # Discover network metrics
    print("\n[3/4] Discovering network metrics...")
    net_count = 0

    try:
        net_io = psutil.net_io_counters(pernic=True)
        # Link state tells the active interface apart from the tunnels, bridges,
        # and container adapters that carry identical-looking counters.
        interface_state = scan_network_interfaces()

        for interface, stats in net_io.items():
            # Skip loopback
            if interface == "lo":
                continue

            info = interface_state.get(interface, {})
            context = {
                "hardware": interface,
                "hardware_detail": info.get("detail", ""),
                "nic_state": info.get("state", "unknown"),
                "is_active_nic": info.get("state") == "primary",
            }

            # Total Data Uploaded
            sensor_database["data"].append({
                "name": f"{interface[:7]}_U",
                "display_name": f"Data Uploaded - {interface}",
                "wmi_sensor_name": "Data Uploaded",
                "source": "psutil_net",
                "type": "data",
                "unit": "MB",
                "interface": interface,
                "metric": "bytes_sent",
                "custom_label": "",
                "current_value": int(stats.bytes_sent / (1024**2)),  # MB
                **context
            })

            # Total Data Downloaded
            sensor_database["data"].append({
                "name": f"{interface[:7]}_D",
                "display_name": f"Data Downloaded - {interface}",
                "wmi_sensor_name": "Data Downloaded",
                "source": "psutil_net",
                "type": "data",
                "unit": "MB",
                "interface": interface,
                "metric": "bytes_recv",
                "custom_label": "",
                "current_value": int(stats.bytes_recv / (1024**2)),  # MB
                **context
            })

            # Upload Speed (will be calculated dynamically)
            sensor_database["throughput"].append({
                "name": f"{interface[:7]}_U",
                "display_name": f"Upload Speed - {interface}",
                "wmi_sensor_name": "Upload Speed",
                "source": "psutil_net_speed",
                "type": "throughput",
                "unit": "MB/s",
                "interface": interface,
                "metric": "bytes_sent",
                "custom_label": "",
                "current_value": 0,  # Will be calculated
                **context
            })

            # Download Speed (will be calculated dynamically)
            sensor_database["throughput"].append({
                "name": f"{interface[:7]}_D",
                "display_name": f"Download Speed - {interface}",
                "wmi_sensor_name": "Download Speed",
                "source": "psutil_net_speed",
                "type": "throughput",
                "unit": "MB/s",
                "interface": interface,
                "metric": "bytes_recv",
                "custom_label": "",
                "current_value": 0,  # Will be calculated
                **context
            })

            net_count += 4

        print(f"  Found {net_count} network metrics:")
        print(f"    - Data: {len(sensor_database['data'])}")
        print(f"    - Throughput: {len(sensor_database['throughput'])}")

    except Exception as e:
        print(f"  WARNING: Could not access network stats: {e}")

    # Discover GPU metrics
    print("\n[4/4] Discovering GPU sensors...")
    nvidia_found = _discover_nvidia_sensors()
    if not nvidia_found:
        _discover_amd_sensors()

    total_gpu = len(sensor_database["gpu"])
    if total_gpu:
        print(f"  Found {total_gpu} GPU metric(s)")
    else:
        print("  No GPU sensors found")
        print("  NVIDIA: pip install nvidia-ml-py3")
        print("  AMD:    requires amdgpu kernel driver (/sys/class/drm/)")

    print("\n" + "=" * 60)
    print("\nℹ NOTE: Sensor values in GUI are static (captured at launch time)")
    print("  This helps you identify active sensors and their typical readings.")


def generate_short_name_linux(label, sensor_type, sensor_key=""):
    """
    Generate a short name (max 10 chars) for ESP32 display - Linux version
    """
    # Start with label or sensor key
    name = label if label else sensor_key

    # Add prefixes based on sensor key
    if sensor_type == "temperature":
        if "coretemp" in sensor_key.lower():
            if "package" in label.lower():
                name = "CPU_PKG"
            elif "core" in label.lower():
                # Extract core number
                import re
                match = re.search(r'core (\d+)', label.lower())
                if match:
                    name = f"CPU_C{match.group(1)}"
                else:
                    name = "CPU_Temp"
            else:
                name = "CPU_Temp"
        elif "k10temp" in sensor_key.lower():
            name = "CPU_Tctl" if "tctl" in label.lower() else "CPU_Temp"
        elif "nvidia" in sensor_key.lower() or "gpu" in sensor_key.lower():
            name = "GPU_Temp"
        elif "nvme" in sensor_key.lower():
            name = "NVMe_Temp"
        else:
            name = label[:10] if label else "TEMP"

    elif sensor_type == "fan":
        if "fan" in label.lower():
            import re
            match = re.search(r'(\d+)', label)
            if match:
                name = f"FAN{match.group(1)}"
            else:
                name = "FAN"
        else:
            name = label[:10] if label else "FAN"

    # Clean up
    name = name.replace(" ", "_").replace("-", "_")

    # Truncate if too long
    if len(name) > 10:
        name = name[:10]

    return name if name else "SENSOR"


# ============================================================================
# GPU Sensor Discovery
# ============================================================================

def _discover_nvidia_sensors():
    """
    Discover NVIDIA GPU sensors via pynvml (nvidia-ml-py3).
    Populates sensor_database["gpu"] and nvml_handles.
    Returns True if any GPU was found.
    """
    global nvml_handles
    try:
        import pynvml
        pynvml.nvmlInit()
        device_count = pynvml.nvmlDeviceGetCount()

        for i in range(device_count):
            handle = pynvml.nvmlDeviceGetHandleByIndex(i)
            nvml_handles[i] = handle

            try:
                gpu_name = pynvml.nvmlDeviceGetName(handle)
                if isinstance(gpu_name, bytes):
                    gpu_name = gpu_name.decode("utf-8")
            except Exception:
                gpu_name = f"NVIDIA GPU {i}"

            prefix = f"GPU{i}" if device_count > 1 else "GPU"

            # GPU Usage %
            try:
                util_val = pynvml.nvmlDeviceGetUtilizationRates(handle).gpu
            except Exception:
                util_val = 0
            sensor_database["gpu"].append({
                "name": f"{prefix}_USE",
                "display_name": f"GPU Usage [{gpu_name}]",
                "source": "nvml_gpu",
                "type": "percent",
                "unit": "%",
                "gpu_index": i,
                "metric": "utilization",
                "custom_label": "",
                "current_value": util_val
            })

            # GPU Memory Used (MB)
            try:
                mem_info = pynvml.nvmlDeviceGetMemoryInfo(handle)
                mem_val = int(mem_info.used / (1024 ** 2))
            except Exception:
                mem_val = 0
            sensor_database["gpu"].append({
                "name": f"{prefix}_MEM",
                "display_name": f"GPU Memory [{gpu_name}]",
                "source": "nvml_gpu",
                "type": "memory",
                "unit": "MB",
                "gpu_index": i,
                "metric": "memory_used",
                "custom_label": "",
                "current_value": mem_val
            })

            # GPU Core Frequency (MHz)
            try:
                freq_val = pynvml.nvmlDeviceGetClockInfo(handle, pynvml.NVML_CLOCK_GRAPHICS)
            except Exception:
                freq_val = 0
            sensor_database["gpu"].append({
                "name": f"{prefix}_FRQ",
                "display_name": f"GPU Freq [{gpu_name}]",
                "source": "nvml_gpu",
                "type": "frequency",
                "unit": "MHz",
                "gpu_index": i,
                "metric": "clock_graphics",
                "custom_label": "",
                "current_value": freq_val
            })

            # GPU Fan Speed (%)
            try:
                fan_val = pynvml.nvmlDeviceGetFanSpeed(handle)
            except Exception:
                fan_val = 0
            sensor_database["gpu"].append({
                "name": f"{prefix}_FAN",
                "display_name": f"GPU Fan [{gpu_name}]",
                "source": "nvml_gpu",
                "type": "percent",
                "unit": "%",
                "gpu_index": i,
                "metric": "fan_speed",
                "custom_label": "",
                "current_value": fan_val
            })

            # GPU Power (W)
            try:
                power_val = int(pynvml.nvmlDeviceGetPowerUsage(handle) / 1000)
            except Exception:
                power_val = 0
            sensor_database["gpu"].append({
                "name": f"{prefix}_PWR",
                "display_name": f"GPU Power [{gpu_name}]",
                "source": "nvml_gpu",
                "type": "power",
                "unit": "W",
                "gpu_index": i,
                "metric": "power_usage",
                "custom_label": "",
                "current_value": power_val
            })

            # GPU Temperature (°C)
            try:
                temp_val = pynvml.nvmlDeviceGetTemperature(handle, pynvml.NVML_TEMPERATURE_GPU)
            except Exception:
                temp_val = 0
            sensor_database["gpu"].append({
                "name": f"{prefix}_TMP",
                "display_name": f"GPU Temp [{gpu_name}]",
                "source": "nvml_gpu",
                "type": "temperature",
                "unit": "C",
                "gpu_index": i,
                "metric": "temperature",
                "custom_label": "",
                "current_value": temp_val
            })

            # GPU Memory Used %
            try:
                mem_info = pynvml.nvmlDeviceGetMemoryInfo(handle)
                mem_pct = int(mem_info.used * 100 / mem_info.total) if mem_info.total > 0 else 0
            except Exception:
                mem_pct = 0
            sensor_database["gpu"].append({
                "name": f"{prefix}_MEMP",
                "display_name": f"GPU Mem % [{gpu_name}]",
                "source": "nvml_gpu",
                "type": "percent",
                "unit": "%",
                "gpu_index": i,
                "metric": "memory_percent",
                "custom_label": "",
                "current_value": mem_pct
            })

        if device_count > 0:
            print(f"  NVIDIA: {device_count} GPU(s) detected via pynvml")
            return True
        return False

    except ImportError:
        print("  NVIDIA: pynvml not installed (pip install nvidia-ml-py3)")
    except Exception as e:
        print(f"  NVIDIA: {e}")

    return False


def _discover_amd_sensors():
    """
    Discover AMD GPU sensors via Linux sysfs (/sys/class/drm/).
    Populates sensor_database["gpu"].
    Returns True if any GPU was found.
    """
    AMD_VENDOR_ID = "0x1002"
    drm_cards = sorted(_glob.glob("/sys/class/drm/card[0-9]"))
    found = False

    def _read_int(path):
        try:
            with open(path) as f:
                return int(f.read().strip())
        except OSError:
            return None

    for card_path in drm_cards:
        device_path = os.path.join(card_path, "device")
        try:
            with open(os.path.join(device_path, "vendor")) as f:
                if f.read().strip().lower() != AMD_VENDOR_ID:
                    continue
        except OSError:
            continue

        card_name = os.path.basename(card_path)          # e.g. "card0"
        gpu_name  = f"AMD GPU ({card_name})"
        prefix    = card_name.upper()                     # e.g. "CARD0"

        hwmon_dirs = sorted(_glob.glob(os.path.join(device_path, "hwmon", "hwmon*")))
        hwmon_path = hwmon_dirs[0] if hwmon_dirs else None

        # GPU Usage %
        busy_val = _read_int(os.path.join(device_path, "gpu_busy_percent"))
        if busy_val is not None:
            sensor_database["gpu"].append({
                "name": f"{prefix}_USE",
                "display_name": f"GPU Usage [{gpu_name}]",
                "source": "amd_sysfs",
                "type": "percent",
                "unit": "%",
                "device_path": device_path,
                "hwmon_path": hwmon_path,
                "metric": "gpu_busy_percent",
                "custom_label": "",
                "current_value": busy_val
            })
            found = True

        # GPU Memory Used (MB)
        vram_val = _read_int(os.path.join(device_path, "mem_info_vram_used"))
        if vram_val is not None:
            sensor_database["gpu"].append({
                "name": f"{prefix}_MEM",
                "display_name": f"GPU Memory [{gpu_name}]",
                "source": "amd_sysfs",
                "type": "memory",
                "unit": "MB",
                "device_path": device_path,
                "hwmon_path": hwmon_path,
                "metric": "mem_info_vram_used",
                "custom_label": "",
                "current_value": int(vram_val / (1024 ** 2))
            })
            found = True

        # GPU Memory Used %
        vram_total_val = _read_int(os.path.join(device_path, "mem_info_vram_total"))
        if vram_val is not None and vram_total_val is not None and vram_total_val > 0:
            sensor_database["gpu"].append({
                "name": f"{prefix}_MEMP",
                "display_name": f"GPU Mem % [{gpu_name}]",
                "source": "amd_sysfs",
                "type": "percent",
                "unit": "%",
                "device_path": device_path,
                "hwmon_path": hwmon_path,
                "metric": "mem_percent",
                "custom_label": "",
                "current_value": int(vram_val * 100 / vram_total_val)
            })
            found = True

        if hwmon_path:
            # GPU Temperature (millidegrees C → °C)
            temp_val = _read_int(os.path.join(hwmon_path, "temp1_input"))
            if temp_val is not None:
                sensor_database["gpu"].append({
                    "name": f"{prefix}_TMP",
                    "display_name": f"GPU Temp [{gpu_name}]",
                    "source": "amd_sysfs",
                    "type": "temperature",
                    "unit": "C",
                    "device_path": device_path,
                    "hwmon_path": hwmon_path,
                    "metric": "temp1_input",
                    "custom_label": "",
                    "current_value": temp_val // 1000
                })
                found = True

            # GPU Core Frequency (Hz → MHz)
            freq_val = _read_int(os.path.join(hwmon_path, "freq1_input"))
            if freq_val is not None:
                sensor_database["gpu"].append({
                    "name": f"{prefix}_FRQ",
                    "display_name": f"GPU Freq [{gpu_name}]",
                    "source": "amd_sysfs",
                    "type": "frequency",
                    "unit": "MHz",
                    "device_path": device_path,
                    "hwmon_path": hwmon_path,
                    "metric": "freq1_input",
                    "custom_label": "",
                    "current_value": freq_val // 1_000_000
                })
                found = True

            # Fan Speed (RPM)
            fan_val = _read_int(os.path.join(hwmon_path, "fan1_input"))
            if fan_val is not None:
                sensor_database["gpu"].append({
                    "name": f"{prefix}_FAN",
                    "display_name": f"GPU Fan [{gpu_name}]",
                    "source": "amd_sysfs",
                    "type": "fan",
                    "unit": "RPM",
                    "device_path": device_path,
                    "hwmon_path": hwmon_path,
                    "metric": "fan1_input",
                    "custom_label": "",
                    "current_value": fan_val
                })
                found = True

            # Power (µW → W); prefer power1_average, fall back to power1_input
            for pwr_metric in ("power1_average", "power1_input"):
                pwr_val = _read_int(os.path.join(hwmon_path, pwr_metric))
                if pwr_val is not None:
                    sensor_database["gpu"].append({
                        "name": f"{prefix}_PWR",
                        "display_name": f"GPU Power [{gpu_name}]",
                        "source": "amd_sysfs",
                        "type": "power",
                        "unit": "W",
                        "device_path": device_path,
                        "hwmon_path": hwmon_path,
                        "metric": pwr_metric,
                        "custom_label": "",
                        "current_value": int(pwr_val / 1_000_000)
                    })
                    found = True
                    break

    if found:
        amd_count = sum(1 for s in sensor_database["gpu"] if s["source"] == "amd_sysfs")
        print(f"  AMD: {amd_count} metric(s) detected via sysfs")
    else:
        print("  AMD: no GPU found (requires amdgpu kernel driver)")

    return found


def get_metric_value(metric_config):
    """
    Get current value for a configured metric - Linux version
    """
    global network_last_sample, network_last_time

    source = metric_config["source"]

    if source == "psutil":
        method = metric_config["psutil_method"]

        if method == "cpu_percent":
            return int(psutil.cpu_percent(interval=0))
        elif method == "virtual_memory.percent":
            return int(psutil.virtual_memory().percent)
        elif method == "virtual_memory.used":
            return int(psutil.virtual_memory().used / (1024**3))  # GB
        elif method == "swap_memory.percent":
            return int(psutil.swap_memory().percent)
        elif method == "swap_memory.used":
            return int(psutil.swap_memory().used / (1024**3))  # GB
        elif method == "disk_usage":
            return int(psutil.disk_usage('/').percent)

    elif source == "psutil_temp":
        try:
            temps = psutil.sensors_temperatures()
            sensor_key = metric_config["sensor_key"]
            sensor_label = metric_config["sensor_label"]

            if sensor_key in temps:
                for entry in temps[sensor_key]:
                    if entry.label == sensor_label:
                        return int(entry.current)
        except:
            pass

    elif source == "psutil_fan":
        try:
            fans = psutil.sensors_fans()
            sensor_key = metric_config["sensor_key"]
            sensor_label = metric_config["sensor_label"]

            if sensor_key in fans:
                for entry in fans[sensor_key]:
                    if entry.label == sensor_label:
                        return int(entry.current)
        except:
            pass

    elif source == "psutil_net":
        try:
            net_io = psutil.net_io_counters(pernic=True)
            interface = metric_config["interface"]
            metric_name = metric_config["metric"]

            if interface in net_io:
                value = getattr(net_io[interface], metric_name)
                return int(value / (1024**2))  # Convert to MB
        except:
            pass

    elif source == "psutil_net_speed":
        try:
            current_time = time.time()
            net_io = psutil.net_io_counters(pernic=True)
            interface = metric_config["interface"]
            metric_name = metric_config["metric"]

            if interface in net_io:
                current_bytes = getattr(net_io[interface], metric_name)

                # Calculate speed if we have previous sample
                key = f"{interface}_{metric_name}"
                if key in network_last_sample and network_last_time:
                    time_delta = current_time - network_last_time
                    bytes_delta = current_bytes - network_last_sample[key]

                    if time_delta > 0:
                        # MB/s
                        speed = (bytes_delta / time_delta) / (1024**2)
                        return int(speed) if speed >= 1 else 0

                # Update last sample
                network_last_sample[key] = current_bytes

                return 0
        except:
            pass

    elif source == "nvml_gpu":
        try:
            import pynvml
            gpu_index = metric_config["gpu_index"]
            # Lazy-init: works even when monitoring starts from a saved config
            if gpu_index not in nvml_handles:
                if not nvml_handles:
                    pynvml.nvmlInit()
                nvml_handles[gpu_index] = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
            handle = nvml_handles[gpu_index]
            metric = metric_config["metric"]

            if metric == "utilization":
                return pynvml.nvmlDeviceGetUtilizationRates(handle).gpu
            elif metric == "memory_used":
                return int(pynvml.nvmlDeviceGetMemoryInfo(handle).used / (1024 ** 2))
            elif metric == "memory_percent":
                info = pynvml.nvmlDeviceGetMemoryInfo(handle)
                return int(info.used * 100 / info.total) if info.total > 0 else 0
            elif metric == "clock_graphics":
                return pynvml.nvmlDeviceGetClockInfo(handle, pynvml.NVML_CLOCK_GRAPHICS)
            elif metric == "fan_speed":
                return pynvml.nvmlDeviceGetFanSpeed(handle)
            elif metric == "power_usage":
                return int(pynvml.nvmlDeviceGetPowerUsage(handle) / 1000)
            elif metric == "temperature":
                return pynvml.nvmlDeviceGetTemperature(handle, pynvml.NVML_TEMPERATURE_GPU)
        except Exception:
            pass

    elif source == "amd_sysfs":
        try:
            metric      = metric_config["metric"]
            device_path = metric_config["device_path"]
            hwmon_path  = metric_config.get("hwmon_path")

            if metric == "gpu_busy_percent":
                with open(os.path.join(device_path, metric)) as f:
                    return int(f.read().strip())
            elif metric == "mem_info_vram_used":
                with open(os.path.join(device_path, metric)) as f:
                    return int(int(f.read().strip()) / (1024 ** 2))
            elif metric == "freq1_input":
                with open(os.path.join(hwmon_path, metric)) as f:
                    return int(f.read().strip()) // 1_000_000
            elif metric == "fan1_input":
                with open(os.path.join(hwmon_path, metric)) as f:
                    return int(f.read().strip())
            elif metric in ("power1_average", "power1_input"):
                with open(os.path.join(hwmon_path, metric)) as f:
                    return int(int(f.read().strip()) / 1_000_000)
            elif metric == "temp1_input":
                with open(os.path.join(hwmon_path, metric)) as f:
                    return int(f.read().strip()) // 1000
            elif metric == "mem_percent":
                used  = int(open(os.path.join(device_path, "mem_info_vram_used")).read().strip())
                total = int(open(os.path.join(device_path, "mem_info_vram_total")).read().strip())
                return int(used * 100 / total) if total > 0 else 0
        except Exception:
            pass

    return 0


