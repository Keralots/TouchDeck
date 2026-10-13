import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest import mock

from app_state import AppState
from config_model import normalize_config
import server


def sensor(key, name):
    return {
        "name": name,
        "display_name": name,
        "wmi_identifier": key,
        "source": "test",
        "unit": "%",
        "type": "Load",
    }


class FakeCore:
    DEFAULT_CONFIG = {"version": "4.1", "esp32_ip": "", "udp_port": 4210,
                      "update_interval": 3, "metrics": []}
    SUPPORTS_SOURCE_SELECT = False

    def __init__(self):
        self.sensor_database = {"system": [sensor("cpu", "CPU"), sensor("gpu", "GPU")],
                                "gpu": [], "temperature": [], "fan": [], "load": [],
                                "clock": [], "power": [], "data": [], "throughput": [],
                                "other": []}
        self.saved = None

    def ensure_discovered(self, rescan=False):
        return None

    def source_text(self):
        return "test sensors"

    def source_banner(self):
        return {"level": "ok", "text": "Test sensors ready."}

    def save_config(self, config):
        self.saved = config
        return True

    def load_config(self):
        return self.saved

    def is_autostart_enabled(self):
        return False


class CompanionServerModel(unittest.TestCase):
    def setUp(self):
        self.core = FakeCore()
        self.state = AppState(normalize_config(self.core.DEFAULT_CONFIG))

    def test_selection_reordering_preserves_ids(self):
        server.apply_select(self.core, self.state, ["cpu", "gpu"])
        first = {m["wmi_identifier"]: m["id"] for m in self.state.get_config()["metrics"]}
        server.apply_select(self.core, self.state, ["gpu", "cpu"])
        second = {m["wmi_identifier"]: m["id"] for m in self.state.get_config()["metrics"]}
        self.assertEqual(first, second)

    def test_labels_are_stream_only_and_sized(self):
        server.apply_select(self.core, self.state, ["cpu"])
        server.apply_labels(self.state, {"cpu": "CPU package sensor"})
        self.assertEqual("CPU package sen", self.state.get_config()["metrics"][0]["custom_label"])

    def test_save_does_not_push_an_oled_layout(self):
        server.apply_select(self.core, self.state, ["cpu"])
        result = server.apply_save(self.core, self.state)
        self.assertTrue(result["success"])
        self.assertNotIn("layout", self.core.saved)

    def test_connection_test_reads_firmware_status_without_udp_probe(self):
        with mock.patch.object(server, "_fetch_device_json", return_value={
            "product": "TouchDeck", "version": "1.0.0", "board": "esp32c3"
        }) as fetch:
            result = server.do_test(self.state, {"esp32_ip": ["192.168.0.69"]})
        self.assertTrue(result["compatible"])
        fetch.assert_called_once()

    def test_connection_address_strips_web_scheme(self):
        self.assertEqual("touchdeck.local",
                         server._device_address("http://touchdeck.local/"))
        with self.assertRaises(ValueError):
            server._device_address("touchdeck.local:8080")

    def test_import_migrates_old_layout_and_duplicate_ids(self):
        old = {"version": "4.0", "metrics": [
            dict(sensor("cpu", "CPU"), id=1), dict(sensor("gpu", "GPU"), id=1)
        ], "layout": {"row_mode": 0}}
        result = server.apply_import(self.core, self.state, old)
        self.assertTrue(result["success"])
        self.assertNotIn("layout", self.core.saved)
        self.assertEqual(2, len({m["id"] for m in self.core.saved["metrics"]}))


    def test_connection_save_cannot_restore_a_forgotten_key(self):
        self.state.set_config(dict(self.state.get_config(), pc_key="11" * 32))
        saving = threading.Event()
        forget_waiting = threading.Event()
        release_save = threading.Event()
        lock = threading.Lock()
        saved_keys = []

        class ObservedLock:
            def __enter__(self):
                if saving.is_set():
                    forget_waiting.set()
                lock.acquire()

            def __exit__(self, *args):
                lock.release()

        def save(config):
            if config.get("pc_key"):
                saving.set()
                if not release_save.wait(2):
                    raise RuntimeError("Connection save was not released")
            saved_keys.append(config["pc_key"])
            self.core.saved = config
            return True

        with mock.patch.object(server, "_CONFIG_LOCK", ObservedLock()), \
                mock.patch.object(self.core, "save_config", side_effect=save), \
                ThreadPoolExecutor(max_workers=2) as pool:
            connection = pool.submit(server.apply_connection, self.core, self.state,
                                     {"esp32_ip": ["touchdeck.local"]})
            try:
                self.assertTrue(saving.wait(2))
                forget = pool.submit(server.pair_forget, self.core, self.state)
                self.assertTrue(forget_waiting.wait(2))
            finally:
                release_save.set()
            self.assertTrue(connection.result(timeout=2)["success"])
            self.assertTrue(forget.result(timeout=2)["success"])
        self.assertEqual(["11" * 32, ""], saved_keys)
        self.assertEqual("", self.state.get_config()["pc_key"])
        self.assertEqual("", self.core.saved["pc_key"])
        self.assertEqual("touchdeck.local", self.state.get_config()["esp32_ip"])


if __name__ == "__main__":
    unittest.main()
