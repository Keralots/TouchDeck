import json
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


class TapPairing(unittest.TestCase):
    def setUp(self):
        self.core = FakeCore()
        config = dict(FakeCore.DEFAULT_CONFIG, esp32_ip="deck.local", pc_id="0123456789ab")
        self.state = AppState(normalize_config(config))
        server._TAP.update(state="idle", message="", auto=False, denied=False)

    def test_start_answers_tap_and_waits_until_allowed(self):
        replies = [(200, {"success": True, "token": "t" * 32})]
        with mock.patch.object(server, "_post_device", side_effect=lambda *a: replies.pop(0)) as post, \
                mock.patch.object(server.threading, "Thread") as thread:
            result = server.pair_start(self.core, self.state)
        self.assertTrue(result["tap"])
        post.assert_called_once_with("deck.local", "/api/pc/pair", {"id": "0123456789ab", "mode": "tap"})
        thread.assert_called_once()
        self.assertEqual("waiting", server.pair_status()["state"])
        # A second start while waiting does not ask the device again.
        with mock.patch.object(server, "_post_device") as post:
            self.assertTrue(server.pair_start(self.core, self.state)["tap"])
        post.assert_not_called()

    def test_wait_collects_the_key_after_allow(self):
        replies = [(202, {"success": False}), (202, {"success": False}),
                   (200, {"success": True, "key": "ab" * 32})]
        with mock.patch.object(server, "_TAP_POLL", 0), \
                mock.patch.object(server, "_post_device", side_effect=lambda *a: replies.pop(0)) as post:
            server._tap_wait(self.core, self.state, "deck.local", "0123456789ab", "t" * 32)
        self.assertEqual(3, post.call_count)
        post.assert_called_with("deck.local", "/api/pc/pair/confirm", {"id": "0123456789ab", "token": "t" * 32})
        self.assertEqual("paired", server.pair_status()["state"])
        self.assertEqual("ab" * 32, self.core.saved["pc_key"])

    def test_deny_is_remembered_and_keeps_the_old_key(self):
        self.state.set_config(dict(self.state.get_config(), pc_key="11" * 32))
        with mock.patch.object(server, "_TAP_POLL", 0), \
                mock.patch.object(server, "_post_device", return_value=(403, {"message": "no pairing in progress"})):
            server._tap_wait(self.core, self.state, "deck.local", "0123456789ab", "t" * 32)
        self.assertEqual("failed", server.pair_status()["state"])
        self.assertTrue(server._TAP["denied"])
        self.assertEqual("11" * 32, self.state.get_config()["pc_key"])

    def test_older_firmware_falls_back_to_the_code(self):
        with mock.patch.object(server, "_post_device", return_value=(200, {"success": True, "message": "Enter the code"})):
            result = server.pair_start(self.core, self.state)
        self.assertFalse(result["tap"])
        self.assertEqual("idle", server.pair_status()["state"])

    def test_watch_pairs_again_only_when_the_device_lost_the_key(self):
        self.state.set_config(dict(self.state.get_config(), pc_key="11" * 32))
        stop = mock.Mock()
        for paired, calls in ((True, 0), (False, 1)):
            stop.wait.side_effect = [False, True]
            with mock.patch.object(server, "_fetch_device_json", return_value={"known": True, "paired": paired}) as fetch, \
                    mock.patch.object(server, "pair_start") as start:
                server.watch_pairing(self.core, self.state, stop)
            fetch.assert_called_once_with("deck.local", "/api/pc/status?id=0123456789ab", timeout=3.0)
            self.assertEqual(calls, start.call_count)
        start.assert_called_once_with(self.core, self.state, auto=True)
        # Denied on the device: no more automatic prompts.
        server._TAP.update(auto=True, denied=True)
        stop.wait.side_effect = [False, True]
        with mock.patch.object(server, "_fetch_device_json") as fetch:
            server.watch_pairing(self.core, self.state, stop)
        fetch.assert_not_called()


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
        self.assertEqual("touchdeck.local:8080", server._device_address("touchdeck.local:8080"))
        self.assertEqual("http://tablet.local:8080/api/status",
                         server._device_url("http://tablet.local:8080/", "/api/status"))

    def panel_request(self, action, address="tablet.local:8080", code=""):
        handler = object.__new__(server._Handler)
        handler.CORE = self.core
        handler.STATE = self.state
        handler.path = "/api/panels/" + action
        handler._form = mock.Mock(return_value={"address": [address], "code": [code]})
        handler._json = mock.Mock()
        handler.do_POST()
        return handler._json.call_args.args[0]

    def get_request(self, path):
        handler = object.__new__(server._Handler)
        handler.CORE = self.core
        handler.STATE = self.state
        handler.path = path
        handler._json = mock.Mock()
        handler.do_GET()
        return handler._json.call_args.args[0]

    def test_panel_add_remove_and_limits(self):
        self.assertTrue(self.panel_request("add")["success"])
        self.assertFalse(self.panel_request("add")["success"])
        self.assertFalse(self.panel_request("add", "bad/path")["success"])
        for address in ("second", "third", "fourth"):
            self.assertTrue(self.panel_request("add", address)["success"])
        self.assertFalse(self.panel_request("add", "fifth")["success"])
        self.assertTrue(self.panel_request("remove")["success"])
        self.assertFalse(self.panel_request("remove")["success"])
        self.assertEqual(3, len(self.core.saved["panels"]))

    def test_panel_pairing_endpoints_keep_deck_and_other_panel_keys(self):
        self.state.set_config(dict(self.state.get_config(), pc_key="11" * 32))
        self.panel_request("add")
        self.panel_request("add", "second")
        for action, path in (("pair/start", "/api/pc/pair"),
                             ("pair/cancel", "/api/pc/pair/cancel")):
            with mock.patch.object(server, "_post_device", return_value=(200, {"success": True})) as post:
                self.assertTrue(self.panel_request(action)["success"])
                post.assert_called_once_with("tablet.local:8080", path,
                                             {"id": self.state.get_config()["pc_id"]})
        with mock.patch.object(server, "_post_device", return_value=(200, {
                "success": True, "key": "22" * 32})) as post:
            result = self.panel_request("pair/confirm", code="123 456")
            self.assertTrue(result["success"])
            self.assertNotIn("22" * 32, json.dumps(result))
            post.assert_called_once_with("tablet.local:8080", "/api/pc/pair/confirm",
                                         {"id": self.state.get_config()["pc_id"], "code": "123456"})
        self.assertEqual("22" * 32, self.core.saved["panels"][0]["key"])
        self.assertEqual("", self.core.saved["panels"][1]["key"])
        self.assertTrue(self.panel_request("pair/forget")["success"])
        self.assertEqual("", self.core.saved["panels"][0]["key"])
        self.assertEqual("11" * 32, self.core.saved["pc_key"])

    def test_panel_pairing_errors_do_not_change_config(self):
        self.panel_request("add")
        before = self.state.get_config()
        with mock.patch.object(server, "_post_device") as post:
            self.assertFalse(self.panel_request("pair/confirm", code="123")["success"])
            self.assertFalse(self.panel_request("pair/start", "missing")["success"])
            post.assert_not_called()
        for response in ((200, None), (403, {"success": False}), (200, {"success": True, "key": "z" * 64}),
                         (200, {"success": False, "key": "22" * 32})):
            with mock.patch.object(server, "_post_device", return_value=response):
                self.assertFalse(self.panel_request("pair/confirm", code="123456")["success"])
        with mock.patch.object(server, "_post_device", side_effect=OSError("offline")):
            self.assertFalse(self.panel_request("pair/start")["success"])
        with mock.patch.object(server, "_post_device", return_value=(200, {
                "success": True, "key": "22" * 32})), \
                mock.patch.object(self.core, "save_config", return_value=False):
            self.assertFalse(self.panel_request("pair/confirm", code="123456")["success"])
            self.assertFalse(self.panel_request("remove")["success"])
            self.assertFalse(self.panel_request("add", "second")["success"])
        self.assertEqual(before, self.state.get_config())

    def test_panel_list_hides_keys_export_keeps_them_for_restore(self):
        self.state.set_config(dict(self.state.get_config(), pc_key="11" * 32, panels=[
            {"address": "tablet.local:8080", "key": "22" * 32},
            {"address": "second", "key": ""}, {"address": "third", "key": ""}]))
        self.state.set_reachable(True)
        with mock.patch.object(server, "_fetch_device_json", side_effect=[
                {"product": "TouchDeck", "key": "22" * 32}, {"product": "Other"}, OSError("offline")]):
            result = self.get_request("/api/panels")
        self.assertEqual([True, True, False], [p["reachable"] for p in result["panels"]])
        self.assertEqual([True, False, False], [p["compatible"] for p in result["panels"]])
        self.assertEqual([True, False, False], [p["paired"] for p in result["panels"]])
        self.assertNotIn("key", json.dumps(result))
        self.assertTrue(self.state.status()["deviceReachable"])
        # A backup restores pairing: import replaces the whole config.
        exported = self.get_request("/api/export")
        self.assertEqual("11" * 32, exported["pc_key"])
        self.assertEqual("22" * 32, exported["panels"][0]["key"])

    def test_invalid_panel_status_does_not_stop_listing(self):
        self.panel_request("add")
        self.panel_request("add", "second")
        with mock.patch.object(server, "_fetch_device_json", side_effect=[[], {"product": "TouchDeck"}]):
            result = self.get_request("/api/panels")
        self.assertEqual([False, True], [p["compatible"] for p in result["panels"]])

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
