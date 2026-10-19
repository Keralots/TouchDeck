import json
import threading
import unittest
from types import SimpleNamespace
from unittest import mock

from app_state import AppState
import app_window
import udp_stream


class UdpStreamTest(unittest.TestCase):
    def setUp(self):
        self.config = {"esp32_ip": "deck.local:8080", "udp_port": 4321,
                       "pc_id": "0123456789ab", "pc_key": "",
                       "panels": [{"address": "tablet.local:9000", "key": "22" * 32},
                                  {"address": "second", "key": ""}]}

    def test_identical_payload_and_nonces_survive_a_send_failure(self):
        payload = udp_stream.packet_identity(self.config)
        payload["metrics"] = [{"id": 1, "value": 42}]
        self.assertEqual(4, len(payload["nonces"]))
        self.assertEqual(4211, payload["cmd_port"])
        data = json.dumps(payload).encode("utf-8")
        for failed in range(3):
            sock = mock.Mock()
            effects = [None, None, None]
            effects[failed] = OSError("unreachable")
            sock.sendto.side_effect = effects
            with self.assertRaises(OSError):
                udp_stream.send_packet(sock, self.config, data)
            self.assertEqual([mock.call(data, (host, 4321)) for host in
                              ("deck.local", "tablet.local", "second")], sock.sendto.call_args_list)

    def test_panels_receive_metrics_and_state_when_deck_uses_usb(self):
        for payload in ({"metrics": [{"id": 1, "value": 42}]}, {"pcstate": {"vol": 10}}):
            payload.update(udp_stream.packet_identity(self.config))
            sock = mock.Mock()
            link = mock.Mock()
            link.is_connected.return_value = True
            with mock.patch.object(app_window, "_serial_link", link):
                app_window._send_packet(sock, self.config, payload)
            link.send.assert_called_once_with(payload)
            self.assertEqual(2, sock.sendto.call_count)
            for call in sock.sendto.call_args_list:
                self.assertEqual(payload, json.loads(call.args[0]))
            self.assertEqual([("tablet.local", 4321), ("second", 4321)],
                             [call.args[1] for call in sock.sendto.call_args_list])

    def test_usb_failure_does_not_stop_panels(self):
        sock = mock.Mock()
        link = mock.Mock()
        link.is_connected.return_value = True
        link.send.side_effect = OSError("disconnected")
        with mock.patch.object(app_window, "_serial_link", link), self.assertRaises(OSError):
            app_window._send_packet(sock, self.config, {"pcstate": {"vol": 10}})
        self.assertEqual(2, sock.sendto.call_count)

    def test_panels_can_receive_without_a_deck(self):
        self.config["esp32_ip"] = ""
        sock = mock.Mock()
        with mock.patch.object(app_window, "_serial_link", None):
            app_window._send_packet(sock, self.config, {"metrics": []})
        self.assertEqual(2, sock.sendto.call_count)

    def test_monitor_runs_with_only_panels_configured(self):
        self.config.update(esp32_ip="", metrics=[{"id": 1}])
        state = AppState(self.config)
        core = SimpleNamespace(PYTHONCOM_AVAILABLE=False, use_rest_api=False, STATUS_OK=0,
                               psutil=mock.Mock(), build_snapshot=mock.Mock(return_value={}),
                               collect_metrics=mock.Mock(return_value=(
                                   {"metrics": [{"id": 1, "value": 42}]}, {1: 42}, True, {}, 0)),
                               recover_source=mock.Mock())
        stop = threading.Event()
        sock = mock.Mock()
        with mock.patch.object(app_window, "_core", core), \
                mock.patch.object(app_window, "_state", state), \
                mock.patch.object(app_window, "_serial_link", None), \
                mock.patch.object(app_window, "_stop", stop), \
                mock.patch.object(app_window.socket, "socket", return_value=sock), \
                mock.patch.object(app_window, "_wait_for_metrics", side_effect=lambda *args: stop.set()):
            app_window._monitor_loop()
        self.assertEqual(2, sock.sendto.call_count)
        self.assertEqual({1: 42}, state.get_values())
        self.assertTrue(state.status()["monitoring"])
