import asyncio
import copy
import json
import os
import subprocess
import sys
import threading
import time
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pc_control
import pc_state


class StateTest(unittest.TestCase):
    def test_trimming_and_controls(self):
        state = pc_state.PCState()
        state._publish({"vol": 45.2, "mute": False,
                        "media": {"status": "playing", "title": " \nA\x00\t" + "x" * 60,
                                  "artist": " B\x7f\x85\u200e "}})
        self.assertEqual(state.read(), {"vol": 45, "mute": False,
                         "media": {"status": "playing", "title": "A" + "x" * 47,
                                   "artist": "B"}})

    def test_unknown_keys_omitted_and_volume_clamped(self):
        state = pc_state.PCState()
        self.assertEqual(state.read(), {})
        state._publish({"vol": None, "mute": None, "media": None, "other": 1})
        self.assertEqual(state.read(), {})
        state._publish({"vol": 150, "media": {"status": "changing", "title": "Song"}})
        self.assertEqual(state.read(), {"vol": 100, "media": {"title": "Song"}})
        state._publish({"vol": -5, "mute": False})
        self.assertEqual(state.read(), {"vol": 0, "mute": False})

    def test_changed_event_and_detached_cache(self):
        state = pc_state.PCState()
        source = {"media": {"status": "paused", "title": "Song"}}
        state._publish(source)
        self.assertTrue(state.changed.is_set())
        state.changed.clear()
        state.read()["media"]["title"] = "Changed"
        state._publish(source)
        self.assertFalse(state.changed.is_set())
        state._publish({})
        self.assertTrue(state.changed.is_set())
        self.assertEqual(state.read(), {})

    def test_worker_uses_own_loop_and_early_poll(self):
        polled = threading.Event()
        calls = []

        async def read():
            calls.append((time.monotonic(), threading.get_ident(), asyncio.get_running_loop()))
            polled.set()
            return {"vol": 30}

        reader = SimpleNamespace(read=read)
        state = pc_state.PCState(lambda: reader)
        with mock.patch.object(pc_state, "POLL_INTERVAL", 10):
            state.start()
            try:
                self.assertTrue(polled.wait(1))
                polled.clear()
                started = time.monotonic()
                state.request_poll()
                self.assertTrue(polled.wait(1))
                self.assertGreaterEqual(calls[1][0] - started, 0.15)
                self.assertLess(calls[1][0] - started, 0.8)
                self.assertNotEqual(calls[0][1], threading.get_ident())
                self.assertIs(calls[0][2], calls[1][2])
                self.assertEqual(state.read(), {"vol": 30})
            finally:
                state.stop()
            self.assertFalse(state._thread.is_alive())

    def test_command_requests_poll_only_after_success(self):
        with mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(pc_control, "_run_windows") as windows, \
                mock.patch.object(pc_control, "_run_linux") as linux, \
                mock.patch.object(pc_state.STATE, "request_poll") as poll:
            for cmd in ("volume_up", "volume_down", "volume_mute", "media_next",
                        "media_prev", "media_stop", "media_play_pause"):
                pc_control.run_command(cmd)
            self.assertEqual(poll.call_count, 7)
            windows.side_effect = linux.side_effect = RuntimeError("failed")
            self.assertFalse(pc_control.execute("volume_up")[0])
            self.assertEqual(poll.call_count, 7)


class PacketTest(unittest.TestCase):
    def test_size_guard_drops_text_then_media_without_mutating_cache(self):
        payload = {"padding": "", "pcstate": {"vol": 45, "media": {
            "status": "playing", "title": "\U0001f600" * 48, "artist": "\U0001f600" * 48}}}
        original = copy.deepcopy(payload)
        base = copy.deepcopy(payload)
        base["pcstate"]["media"] = {"status": "playing"}
        payload["padding"] = "x" * (1899 - len(json.dumps(base).encode("utf-8")))
        bounded, data = pc_state.encode_packet(payload)
        self.assertEqual(len(data), 1899)
        self.assertEqual(bounded["pcstate"]["media"], {"status": "playing"})
        self.assertEqual(payload["pcstate"], original["pcstate"])
        payload["padding"] += "x"
        bounded, data = pc_state.encode_packet(payload)
        self.assertLess(len(data), 1900)
        self.assertEqual(bounded["pcstate"], {"vol": 45})
        payload["padding"] += "x" * 100
        bounded, data = pc_state.encode_packet(payload)
        self.assertNotIn("pcstate", bounded)
        self.assertNotIn(b"pcstate", data)

    def test_small_packet_preserved(self):
        payload = {"pcstate": {"vol": 20, "mute": True}, "metrics": []}
        bounded, data = pc_state.encode_packet(payload)
        self.assertEqual(json.loads(data), payload)
        self.assertEqual(bounded, payload)


class LinuxTest(unittest.TestCase):
    def test_pactl_and_playerctl_output(self):
        output = ["Volume: front-left: 29491 /  45% / -20.81 dB,   front-right: 29491 / 45%",
                  "Mute: yes\n", "Playing\n", "An artist\tA title\n"]
        with mock.patch.object(pc_state.subprocess, "run") as run:
            run.side_effect = [SimpleNamespace(stdout=value) for value in output]
            state = asyncio.run(pc_state._LinuxReader().read())
        self.assertEqual(state, {"vol": 45, "mute": True, "media": {
            "status": "playing", "artist": "An artist", "title": "A title"}})
        self.assertEqual(run.call_args.args[0],
                         ("playerctl", "metadata", "--format", "{{artist}}\t{{title}}"))
        for call in run.call_args_list:
            self.assertEqual(call.kwargs["timeout"], pc_state.TOOL_TIMEOUT)
            self.assertEqual(call.kwargs["env"]["LC_ALL"], "C")

    def test_no_player_and_bad_volume_are_unknown(self):
        with mock.patch.object(pc_state.subprocess, "run") as run:
            run.side_effect = [SimpleNamespace(stdout="bad volume"),
                               SimpleNamespace(stdout="Mute: no"),
                               subprocess.CalledProcessError(1, "playerctl")]
            self.assertEqual(asyncio.run(pc_state._LinuxReader().read()), {"mute": False})

    def test_missing_tools_logged_once(self):
        reader = pc_state._LinuxReader()
        with mock.patch.object(pc_state.subprocess, "run", side_effect=FileNotFoundError), \
                self.assertLogs("pc_state", level="WARNING") as logs:
            self.assertEqual(asyncio.run(reader.read()), {})
            self.assertEqual(asyncio.run(reader.read()), {})
        self.assertEqual(len(logs.output), 2)

    def test_timeouts_are_unknown(self):
        with mock.patch.object(pc_state.subprocess, "run",
                               side_effect=subprocess.TimeoutExpired("pactl", 0.2)):
            self.assertEqual(asyncio.run(pc_state._LinuxReader().read()), {})


class WindowsTest(unittest.TestCase):
    def reader(self):
        with mock.patch.dict(sys.modules, {"comtypes": mock.Mock(),
                             "pycaw": mock.Mock(),
                             "winrt.windows.media.control": mock.Mock()}):
            return pc_state._WindowsReader()

    def test_endpoint_change_and_failure_reacquire(self):
        reader = self.reader()
        first, second = mock.Mock(), mock.Mock()
        first.GetMasterVolumeLevelScalar.return_value = 0.45
        first.GetMute.return_value = 0
        second.GetMasterVolumeLevelScalar.return_value = 0.7
        second.GetMute.return_value = 1
        device = mock.Mock()
        device.GetId.return_value = "first"
        device.Activate.return_value.QueryInterface.return_value = first
        enumerator = reader._com.CoCreateInstance.return_value
        enumerator.GetDefaultAudioEndpoint.return_value = device
        self.assertEqual(reader._volume(), {"vol": 45, "mute": False})
        self.assertEqual(reader._volume(), {"vol": 45, "mute": False})
        self.assertEqual(device.Activate.call_count, 1)
        device.GetId.return_value = "second"
        device.Activate.return_value.QueryInterface.return_value = second
        self.assertEqual(reader._volume(), {"vol": 70, "mute": True})
        second.GetMute.side_effect = OSError("device lost")
        self.assertEqual(reader._volume(), {})
        self.assertIsNone(reader._enumerator)
        device.Activate.return_value.QueryInterface.return_value = first
        self.assertEqual(reader._volume(), {"vol": 45, "mute": False})
        self.assertEqual(reader._com.CoCreateInstance.call_count, 2)
        reader.close()

    def test_media_status_and_no_session(self):
        reader = self.reader()
        session = mock.Mock()
        session.try_get_media_properties_async = mock.AsyncMock(
            return_value=SimpleNamespace(title="Song", artist="Artist"))
        reader._manager = mock.Mock()
        reader._manager.get_current_session.return_value = session
        for code, status in ((3, "stopped"), (4, "playing"), (5, "paused")):
            session.get_playback_info.return_value = SimpleNamespace(playback_status=code)
            self.assertEqual(asyncio.run(reader._media()),
                             {"status": status, "title": "Song", "artist": "Artist"})
        session.get_playback_info.return_value = SimpleNamespace(playback_status=2)
        self.assertNotIn("status", asyncio.run(reader._media()))
        reader._manager.get_current_session.return_value = None
        self.assertIsNone(asyncio.run(reader._media()))
        reader.close()

    def test_optional_imports(self):
        with mock.patch.dict(sys.modules, {"comtypes": None,
                             "winrt.windows.media.control": None}), \
                self.assertLogs("pc_state", level="WARNING") as logs:
            reader = pc_state._WindowsReader()
            self.assertEqual(asyncio.run(reader.read()), {})
            self.assertEqual(asyncio.run(reader.read()), {})
            reader.close()
        self.assertEqual(len(logs.output), 2)


class SendLoopTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import app_window
        cls.app = app_window

    def test_early_send_rate_limit_and_deadline(self):
        app = self.app
        state = pc_state.PCState()
        state._publish({"vol": 10})
        clock = [0.0]
        sends = []
        stop = mock.Mock()
        stop.wait.side_effect = lambda delay: clock.__setitem__(0, clock[0] + delay) or False
        cfg = {"pc_id": "pc", "pc_name": "My PC", "pc_key": "11" * 32, "cmd_port": 4211}

        def send(sock, config, payload):
            sends.append((clock[0], payload))
            state._publish({"vol": len(sends) + 10})

        with mock.patch.object(app, "_stop", stop), \
                mock.patch.object(app, "_state") as config, \
                mock.patch.object(pc_state, "STATE", state), \
                mock.patch.object(app.time, "monotonic", side_effect=lambda: clock[0]), \
                mock.patch.object(app, "_send_packet", side_effect=send):
            config.get_config.return_value = cfg
            next_send = app._wait_for_metrics(None, 0.6, 0)
            app._wait_for_metrics(None, 1.0, next_send)
        self.assertEqual([round(t, 2) for t, _ in sends], [0, 0.25, 0.5, 0.75])
        self.assertEqual(clock[0], 1.0)
        for _, payload in sends:
            self.assertEqual(set(payload), {"pc_id", "pc_name", "cmd_port", "nonces", "pcstate"})
            self.assertEqual(len(payload["nonces"]), 4)
        self.assertEqual(app._packet_identity({"pc_id": "pc"}), {"pc_id": "pc", "pc_name": ""})

    def test_stop_wakes_idle_wait(self):
        app = self.app
        stop = threading.Event()
        state = pc_state.PCState()
        with mock.patch.object(app, "_stop", stop), mock.patch.object(pc_state, "STATE", state):
            worker = threading.Thread(target=app._wait_for_metrics,
                                      args=(None, time.monotonic() + 10, 0))
            worker.start()
            stop.set()
            worker.join(0.3)
            self.assertFalse(worker.is_alive())

    def test_transport_and_size_guard_shared(self):
        app = self.app
        cfg = {"esp32_ip": "192.0.2.1", "udp_port": 4210}
        payload = {"pc_id": "pc", "pcstate": {"vol": 45}}
        sock = mock.Mock()
        link = mock.Mock()
        with mock.patch.object(app, "_serial_link", link):
            link.is_connected.return_value = True
            app._send_packet(sock, cfg, payload)
            link.send.assert_called_once_with(payload)
            sock.sendto.assert_not_called()
            link.is_connected.return_value = False
            app._send_packet(sock, cfg, payload)
            self.assertEqual(json.loads(sock.sendto.call_args.args[0]), payload)
            app._send_packet(sock, cfg, {"pc_name": "x" * 1900})
            self.assertEqual(sock.sendto.call_count, 2)

    def test_metrics_pacing_with_frequent_state_changes(self):
        app = self.app
        clock = [0.0]
        stopped = [False]
        sent = []
        state = pc_state.PCState()
        state._publish({"vol": 10})
        cfg = {"metrics": [{"id": 1}], "update_interval": 3, "pc_id": "pc"}
        config = mock.Mock()
        config.get_config.return_value = cfg
        config.generation.return_value = 1
        stop = mock.Mock()
        stop.is_set.side_effect = lambda: stopped[0]

        def wait(delay):
            clock[0] += delay
            if delay:
                state._publish({"vol": round(clock[0] * 10)})
            return stopped[0]

        def send(payload):
            sent.append((clock[0], payload))
            if sum("metrics" in packet for _, packet in sent) == 3:
                stopped[0] = True

        stop.wait.side_effect = wait
        core = mock.Mock(PYTHONCOM_AVAILABLE=False, use_rest_api=False, STATUS_OK=0)
        core.collect_metrics.side_effect = lambda *args: (
            {"metrics": [{"id": 1, "v": 42}], "status": 0, "timestamp": clock[0]},
            {1: 42}, None, {}, None)
        link = mock.Mock()
        link.is_connected.return_value = True
        link.send.side_effect = send
        with mock.patch.object(app, "_core", core), \
                mock.patch.object(app, "_state", config), \
                mock.patch.object(app, "_stop", stop), \
                mock.patch.object(app, "_serial_link", link), \
                mock.patch.object(pc_state, "STATE", state), \
                mock.patch.object(state.changed, "wait", side_effect=wait), \
                mock.patch.object(app.time, "monotonic", side_effect=lambda: clock[0]), \
                mock.patch.object(app.time, "time", side_effect=lambda: clock[0]), \
                mock.patch.object(app.socket, "socket"):
            app._monitor_loop()
        regular = [(t, packet) for t, packet in sent if "metrics" in packet]
        early = [(t, packet) for t, packet in sent if "metrics" not in packet]
        self.assertEqual([round(t, 6) for t, _ in regular], [0, 3, 6])
        self.assertTrue(early)
        for _, packet in early:
            self.assertEqual(set(packet), {"pc_id", "pc_name", "pcstate"})
        for (before, _), (after, _) in zip(early, early[1:]):
            self.assertGreaterEqual(after - before + 1e-9, 0.25)
        for _, packet in regular:
            self.assertIn("pcstate", packet)

    def test_empty_state_sent_to_clear_removed_session(self):
        app = self.app
        state = pc_state.PCState()
        state._publish({"media": {"status": "playing"}})
        state._publish({})
        stop = threading.Event()
        with mock.patch.object(app, "_stop", stop), \
                mock.patch.object(app, "_state") as config, \
                mock.patch.object(pc_state, "STATE", state), \
                mock.patch.object(app, "_send_packet", side_effect=lambda *args: stop.set()) as send:
            config.get_config.return_value = {"pc_id": "pc"}
            app._wait_for_metrics(None, time.monotonic() + 1, 0)
        self.assertEqual(send.call_args.args[2], {"pc_id": "pc", "pc_name": "", "pcstate": {}})


if __name__ == "__main__":
    unittest.main()
