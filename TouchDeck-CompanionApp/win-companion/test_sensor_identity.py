"""Tests for the hardware context that makes look-alike sensors identifiable."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "companion-common"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import touchdeck_companion as core


# A trimmed copy of the LibreHardwareMonitor REST tree: root, computer, device,
# sensor-type group, sensor.
TREE = {
    "Text": "Sensor",
    "Children": [{
        "Text": "PC7",
        "Children": [
            {
                "Text": "Samsung SSD 980 PRO 1TB",
                "Children": [{
                    "Text": "Temperatures",
                    "Children": [
                        {"Text": "Temperature", "SensorId": "/nvme/2/temperature/0", "Value": "48.0 C"},
                    ],
                }],
            },
            {
                "Text": "Wi-Fi",
                "Children": [{
                    "Text": "Load",
                    "Children": [
                        {"Text": "Network Utilization", "SensorId": "/nic/%7BA%7D/load/1", "Value": "0.0 %"},
                    ],
                }],
            },
            {
                "Text": "MSI MAG Z790 TOMAHAWK WIFI",
                "Children": [{
                    "Text": "Nuvoton NCT6687D",
                    "Children": [{
                        "Text": "Fans",
                        "Children": [
                            {"Text": "Fan #2", "SensorId": "/lpc/nct6687d/0/fan/1", "Value": "900 RPM"},
                        ],
                    }],
                }],
            },
        ],
    }],
}


def _by_id(sensors):
    return {sensor["SensorId"]: sensor for sensor in sensors}


class SensorTreeHardware(unittest.TestCase):
    def test_device_name_wins_over_sensor_type_group(self):
        sensors = _by_id(core.extract_sensors_from_tree(TREE))
        self.assertEqual(sensors["/nvme/2/temperature/0"]["_parent_hardware"],
                         "Samsung SSD 980 PRO 1TB")
        self.assertEqual(sensors["/nic/%7BA%7D/load/1"]["_parent_hardware"], "Wi-Fi")

    def test_nested_hardware_wins_over_the_board(self):
        sensors = _by_id(core.extract_sensors_from_tree(TREE))
        self.assertEqual(sensors["/lpc/nct6687d/0/fan/1"]["_parent_hardware"],
                         "Nuvoton NCT6687D")

    def test_every_sensor_is_collected(self):
        self.assertEqual(len(core.extract_sensors_from_tree(TREE)), 3)


class HardwareDescription(unittest.TestCase):
    def setUp(self):
        self._saved = dict(core._hardware_context)
        core._hardware_context["nics"] = {
            "ethernet": {"name": "Ethernet", "state": "primary",
                         "ipv4": "192.168.0.10", "speed": 2500},
            "wi-fi": {"name": "Wi-Fi", "state": "down", "ipv4": "", "speed": 0},
        }
        core._hardware_context["disks"] = {2: ["C:"], 4: ["D:", "E:"]}

    def tearDown(self):
        core._hardware_context.clear()
        core._hardware_context.update(self._saved)

    def test_primary_interface_is_marked_and_described(self):
        hardware, detail, state = core.describe_hardware("/nic/%7BA%7D/load/1", "Ethernet")
        self.assertEqual(hardware, "Ethernet")
        self.assertEqual(state, "primary")
        self.assertEqual(detail, "192.168.0.10 - 2.5 Gb/s")

    def test_disconnected_interface_reports_no_address(self):
        _hardware, detail, state = core.describe_hardware("/nic/%7BB%7D/load/1", "Wi-Fi")
        self.assertEqual(state, "down")
        self.assertEqual(detail, "no address")

    def test_interface_missing_from_the_os_is_flagged(self):
        _hardware, _detail, state = core.describe_hardware("/nic/%7BC%7D/load/1", "Old Adapter")
        self.assertEqual(state, "unknown")

    def test_disks_carry_drive_letters_and_index(self):
        _hardware, detail, state = core.describe_hardware("/nvme/2/temperature/0",
                                                          "Samsung SSD 980 PRO 1TB")
        self.assertEqual(detail, "C: - disk 2")
        self.assertEqual(state, "")

    def test_identical_drives_stay_distinct_without_letters(self):
        first = core.describe_hardware("/hdd/0/temperature/0", "ST4000VN008")[1]
        second = core.describe_hardware("/hdd/1/temperature/0", "ST4000VN008")[1]
        self.assertEqual(first, "disk 0")
        self.assertEqual(second, "disk 1")
        self.assertNotEqual(first, second)


class DisplayNames(unittest.TestCase):
    def test_bare_name_is_qualified_with_the_device(self):
        self.assertEqual(
            core.build_display_name("Temperature", "Samsung SSD 980 PRO 1TB", "/nvme/2/temperature/0"),
            "Temperature [Samsung SSD 980 PRO 1TB]")

    def test_device_is_not_repeated(self):
        self.assertEqual(core.build_display_name("Wi-Fi Signal", "Wi-Fi", "/nic/%7BA%7D/load/2"),
                         "Wi-Fi Signal")

    def test_identifier_is_the_fallback_when_no_device_is_known(self):
        self.assertEqual(core.build_display_name("Temperature", "", "/nvme/2/temperature/0"),
                         "Temperature [nvme]")

    def test_unnamed_nic_counters_get_a_direction(self):
        self.assertEqual(core._direction_qualified_name("Throughput", "throughput", "0"),
                         "Throughput - Upload")
        self.assertEqual(core._direction_qualified_name("Data", "data", "0"),
                         "Data - Download")
        self.assertEqual(core._direction_qualified_name("Upload Speed", "throughput", "0"),
                         "Upload Speed")


class WmiHardwareLookup(unittest.TestCase):
    def test_longest_identifier_prefix_wins(self):
        names = {"/lpc/nct6687d/0": "Nuvoton NCT6687D", "/lpc": "Motherboard"}
        self.assertEqual(core._wmi_hardware_name("/lpc/nct6687d/0/fan/1", names),
                         "Nuvoton NCT6687D")

    def test_unknown_identifier_returns_empty(self):
        self.assertEqual(core._wmi_hardware_name("/nvme/2/temperature/0", {}), "")


if __name__ == "__main__":
    unittest.main()
