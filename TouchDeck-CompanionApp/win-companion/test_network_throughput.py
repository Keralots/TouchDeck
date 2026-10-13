"""psutil network discovery, wire units, and persistent selection."""

import os
import socket
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "companion-common"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import touchdeck_companion as core
from app_state import AppState
from config_model import clean_stream_label, normalize_config, sensor_key
import server


class NetworkThroughput(unittest.TestCase):
    def setUp(self):
        self.enterContext(mock.patch.object(core, "_network_baselines", {}))
        self.counters = self.enterContext(mock.patch.object(core.psutil, "net_io_counters"))
        self.clock = self.enterContext(mock.patch.object(core.time, "monotonic"))

    def read(self, now, received, sent=0, direction="recv", nic="Ethernet"):
        self.clock.return_value = now
        self.counters.return_value = {
            nic: SimpleNamespace(bytes_recv=received, bytes_sent=sent)
        }
        return core.get_metric_value({"source": "psutil",
                                      "psutil_method": f"net_{direction}:{nic}"})

    def test_first_read_is_zero(self):
        self.assertEqual(self.read(10, 123456789), 0)
        self.counters.assert_called_once_with(pernic=True, nowrap=False)

    def test_one_mib_per_second_in_both_directions(self):
        for direction in ("recv", "sent"):
            with self.subTest(direction=direction):
                self.assertEqual(self.read(10, 100, 100, direction), 0)
                self.assertEqual(self.read(12, 100 + 2 * 1024**2,
                                           100 + 2 * 1024**2, direction), 10240)
                self.assertEqual(self.read(15, 100 + 5 * 1024**2,
                                           100 + 5 * 1024**2, direction), 10240)

    def test_rounds_fractional_tenths(self):
        self.read(10, 0)
        self.assertEqual(self.read(12, 1000), 5)

    def test_counter_reset_rebaselines(self):
        self.read(10, 100000)
        self.assertEqual(self.read(11, 10), 0)
        self.assertEqual(self.read(12, 10 + 1024**2), 10240)

    def test_missing_nic_clears_baseline(self):
        self.read(10, 100)
        self.counters.return_value = {}
        metric = {"source": "psutil", "psutil_method": "net_recv:Ethernet"}
        self.assertEqual(core.get_metric_value(metric), 0)
        self.assertEqual(core.get_metric_value(metric), 0)
        self.assertEqual(self.read(20, 100000000), 0)
        self.assertEqual(self.read(21, 100000000 + 1024), 10)

    def test_nonpositive_elapsed_time_is_zero(self):
        self.read(10, 0)
        self.assertEqual(self.read(10, 1024), 0)
        self.assertEqual(self.read(9, 2048), 0)
        self.assertEqual(self.read(10, 3072), 10)

    def test_interfaces_and_directions_have_independent_baselines(self):
        self.read(10, 100, 900)
        self.assertEqual(self.read(11, 100, 900, "sent"), 0)
        self.assertEqual(self.read(12, 5000, nic="Wi-Fi"), 0)
        self.assertEqual(self.read(12, 100 + 2048, 900), 10)
        self.assertEqual(self.read(13, 0, 900 + 4096, "sent"), 20)
        self.assertEqual(self.read(14, 5000 + 6144, nic="Wi-Fi"), 30)

    def test_nic_name_can_contain_a_colon(self):
        self.read(10, 100, nic="USB: Ethernet")
        self.assertEqual(self.read(11, 1124, nic="USB: Ethernet"), 10)

    def test_counter_error_returns_none(self):
        self.counters.side_effect = OSError("unavailable")
        self.assertIsNone(core.get_metric_value({
            "source": "psutil", "psutil_method": "net_recv:Ethernet"}))

    def test_existing_psutil_methods(self):
        with mock.patch.object(core.psutil, "cpu_percent", return_value=42.8), \
                mock.patch.object(core.psutil, "virtual_memory", return_value=
                                  SimpleNamespace(percent=64.2, used=3 * 1024**3)), \
                mock.patch.object(core.psutil, "disk_usage", return_value=
                                  SimpleNamespace(percent=51.9)):
            for method, expected in (("cpu_percent", 42), ("virtual_memory.percent", 64),
                                     ("virtual_memory.used", 3), ("disk_usage", 51),
                                     ("unknown", None)):
                with self.subTest(method=method):
                    self.assertEqual(core.get_metric_value({
                        "source": "psutil", "psutil_method": method}), expected)
        self.counters.assert_not_called()


class NetworkDiscovery(unittest.TestCase):
    def setUp(self):
        self.stats = {
            "Ethernet": SimpleNamespace(isup=True, flags="", speed=2500),
            "Wi-Fi": SimpleNamespace(isup=False, flags="", speed=0),
            "vEthernet (Default Switch)": SimpleNamespace(isup=True, flags=""),
            "Loopback Pseudo-Interface 1": SimpleNamespace(isup=True, flags=""),
            "Flagged adapter": SimpleNamespace(isup=True, flags="up,loopback,running"),
            "Renamed loop": SimpleNamespace(isup=True, flags=""),
            "IPv6 loop": SimpleNamespace(isup=True, flags=""),
        }
        self.net_stats = self.enterContext(mock.patch.object(
            core.psutil, "net_if_stats", return_value=self.stats))
        self.addresses = self.enterContext(mock.patch.object(
            core.psutil, "net_if_addrs", return_value={
                "Ethernet": [SimpleNamespace(family=socket.AF_INET, address="192.168.0.10")],
                "Renamed loop": [SimpleNamespace(family=socket.AF_INET, address="127.0.0.1")],
                "IPv6 loop": [SimpleNamespace(family=socket.AF_INET6, address="::1")],
            }))

    def test_only_up_non_loopback_interfaces_are_discovered(self):
        sensors = core.discover_network_sensors()
        self.assertEqual({s["psutil_method"] for s in sensors}, {
            "net_recv:Ethernet", "net_sent:Ethernet",
            "net_recv:vEthernet (Default Switch)", "net_sent:vEthernet (Default Switch)"})
        self.assertEqual(len(sensors), 4)
        for sensor in sensors:
            self.assertEqual(sensor["source"], "psutil")
            self.assertEqual(sensor["type"], "throughput")
            self.assertEqual(sensor["unit"], "KB/s")
            self.assertEqual(sensor["current_value"], 0)
            self.assertEqual(sensor["custom_label"], "")
            self.assertEqual(clean_stream_label(sensor["name"]), sensor["name"])
            self.assertLessEqual(len(sensor["name"].encode("ascii")), 16)

    def test_identity_survives_order_speed_and_address_changes(self):
        first = {s["psutil_method"]: sensor_key(s) for s in core.discover_network_sensors()}
        self.net_stats.return_value = dict(reversed(list(self.stats.items())))
        self.stats["Ethernet"].speed = 1000
        self.addresses.return_value["Ethernet"][0].address = "10.0.0.2"
        second = {s["psutil_method"]: sensor_key(s) for s in core.discover_network_sensors()}
        self.assertEqual(first, second)
        self.assertEqual(first["net_recv:Ethernet"],
                         "psutil_Download Speed [Ethernet] (psutil)")
        self.assertEqual(first["net_sent:Ethernet"],
                         "psutil_Upload Speed [Ethernet] (psutil)")
        self.assertEqual(len(set(first.values())), 4)

    def test_discovery_and_api_selection_keep_throughput_category_and_ids(self):
        lhm_sensor = {"wmi_identifier": "/nic/0/throughput/0", "source": "wmi"}
        database = {category: [] for category in core.sensor_database}
        database["throughput"].append(lhm_sensor)
        state = AppState(normalize_config(core.DEFAULT_CONFIG))
        with mock.patch.object(core, "sensor_database", database), \
                mock.patch.object(core, "refresh_hardware_context"), \
                mock.patch.object(core, "use_rest_api", True), \
                mock.patch.object(core, "discover_sensors_via_http", return_value=False), \
                mock.patch.object(core.psutil, "cpu_percent", return_value=0), \
                mock.patch.object(core.psutil, "virtual_memory", return_value=
                                  SimpleNamespace(percent=0, used=0)), \
                mock.patch.object(core.psutil, "disk_usage", return_value=
                                  SimpleNamespace(percent=0)), \
                mock.patch.object(core, "ensure_discovered"):
            core.discover_sensors()
            self.assertIs(database["throughput"][0], lhm_sensor)
            rows = [s for s in server.flatten_sensors(core, state.get_config())
                    if s["source"] == "psutil" and s["type"] == "throughput"]
            self.assertEqual(len(rows), 4)
            self.assertTrue(all(s["category"] == "throughput" for s in rows))
            keys = [s["key"] for s in rows]
            handler = SimpleNamespace(
                CORE=core, STATE=state, path="/api/select",
                _json_body=lambda: {"keys": keys}, _json=lambda result: result)
            self.assertEqual(server._Handler.do_POST(handler), {"success": True, "count": 4})
            metrics = state.get_config()["metrics"]
            first_ids = {sensor_key(m): m["id"] for m in metrics}
            self.assertEqual({m["psutil_method"] for m in metrics},
                             {s["psutil_method"] for s in core.discover_network_sensors()})
            server.apply_select(core, state, [])
            database["throughput"] = [lhm_sensor] + list(reversed(core.discover_network_sensors()))
            server._Handler.do_POST(handler)
            self.assertEqual(first_ids, {sensor_key(m): m["id"]
                                         for m in state.get_config()["metrics"]})


if __name__ == "__main__":
    unittest.main()
