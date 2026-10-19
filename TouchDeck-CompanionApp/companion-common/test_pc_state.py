import asyncio
import copy
from datetime import datetime, timedelta, timezone
from pathlib import Path
import tempfile
import zlib
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


class WinrtCallTest(unittest.TestCase):
    def test_result_timeout_and_stuck_call(self):
        async def value():
            return 7
        self.assertEqual(pc_state.run_winrt("t1", value, 1.0), 7)
        release = threading.Event()

        async def stuck():
            await asyncio.get_running_loop().run_in_executor(None, release.wait)
            return 1
        self.assertIsNone(pc_state.run_winrt("t2", stuck, 0.05))
        # The stuck call still runs: a new one with the same key is skipped, not stacked.
        self.assertIsNone(pc_state.run_winrt("t2", value, 1.0))
        release.set()
        pc_state._winrt_busy["t2"].join(1.0)
        self.assertEqual(pc_state.run_winrt("t2", value, 1.0), 7)


class PacketTest(unittest.TestCase):
    def test_size_guard_drops_text_then_media_without_mutating_cache(self):
        payload = {"padding": "", "pcstate": {"vol": 45, "media": {
            "status": "playing", "title": "\U0001f600" * 48, "artist": "\U0001f600" * 48}}}
        original = copy.deepcopy(payload)
        base = copy.deepcopy(payload)
        base["pcstate"]["media"] = {"status": "playing"}
        limit = pc_state.PACKET_LIMIT
        compact = json.dumps(base, separators=(",", ":")).encode("utf-8")
        payload["padding"] = "x" * (limit - len(compact))
        bounded, data = pc_state.encode_packet(payload)
        self.assertEqual(len(data), limit)
        self.assertEqual(bounded["pcstate"]["media"], {"status": "playing"})
        self.assertEqual(payload["pcstate"], original["pcstate"])
        payload["padding"] += "x"
        bounded, data = pc_state.encode_packet(payload)
        self.assertLessEqual(len(data), limit)
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
                  "Mute: yes\n", "Paused\n", "An artist\tA title\t180000000\t\n", "12.5\n"]
        with mock.patch.object(pc_state.subprocess, "run") as run:
            run.side_effect = [SimpleNamespace(stdout=value) for value in output]
            state = asyncio.run(pc_state._LinuxReader().read())
        self.assertEqual(state, {"vol": 45, "mute": True, "media": {
            "status": "paused", "artist": "An artist", "title": "A title", "dur": 180, "pos": 12}})
        self.assertEqual(run.call_args.args[0], ("playerctl", "position"))
        self.assertEqual(run.call_args_list[3].args[0],
                         ("playerctl", "metadata", "--format",
                          "{{artist}}\t{{title}}\t{{mpris:length}}\t{{mpris:artUrl}}"))
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
        reader._manager.get_sessions.return_value = []
        session.source_app_user_model_id = "player"
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


class TimelineTest(unittest.TestCase):
    def test_playing_origin_and_position_stay_stable_until_seek(self):
        timeline = pc_state._Timeline()
        state = pc_state.PCState()
        first = {"status": "playing"}
        timeline.apply(first, "track", 180, 10, 1000)
        state._publish({"media": first})
        state.changed.clear()
        for position, now in ((11, 1001), (12, 1003), (14, 1004)):
            media = {"status": "playing"}
            timeline.apply(media, "track", 180, position, now)
            self.assertEqual(media, first)
            state._publish({"media": media})
            self.assertFalse(state.changed.is_set())
        media = {"status": "playing"}
        timeline.apply(media, "track", 180, 60, 1005)
        self.assertEqual((media["pos"], media["t0"]), (60, 945))
        state._publish({"media": media})
        self.assertTrue(state.changed.is_set())

    def test_pause_resume_track_change_and_clamp(self):
        timeline = pc_state._Timeline()
        for status, key, pos, now, expected in (
                ("playing", "one", 4, 100, {"pos": 4, "t0": 96}),
                ("paused", "one", 5.9, 101, {"pos": 5}),
                ("paused", "one", 6.1, 102, {"pos": 6}),
                ("playing", "one", 6.1, 103, {"pos": 6, "t0": 97}),
                ("playing", "two", 7, 104, {"pos": 7, "t0": 97}),
                ("stopped", "two", -4, 105, {"pos": 0}),
                ("paused", "two", 999, 106, {"pos": 180})):
            media = {"status": status}
            timeline.apply(media, key, 180, pos, now)
            self.assertEqual(media, dict(expected, status=status, dur=180))

    def test_clean_new_fields(self):
        media = {"status": "playing", "dur": 180, "pos": 0,
                 "t0": 1700000000, "art": "A012B3FF"}
        self.assertEqual(pc_state._clean({"media": media}),
                         {"media": dict(media, art="a012b3ff")})
        for key, bad in (("dur", 0), ("dur", -1), ("pos", "3"), ("pos", True),
                         ("t0", 2147483648), ("art", "abc"), ("art", "zzzzzzzz")):
            cleaned = pc_state._clean({"media": dict(media, **{key: bad})})
            self.assertNotIn(key, cleaned["media"])
        self.assertNotIn("t0", pc_state._clean({"media": dict(media, status="paused")})["media"])

    def test_packet_keeps_timing_and_art_before_text(self):
        media = {"status": "playing", "dur": 200, "pos": 10,
                 "t0": 1700000000, "art": "0123abcd"}
        packet = {"art_port": 8751, "padding": "x" * 1100, "pcstate": {"media":
                  dict(media, title="x" * 200, artist="y" * 200)}}
        result, data = pc_state.encode_packet(packet)
        self.assertLessEqual(len(data), pc_state.PACKET_LIMIT)
        self.assertEqual(result["pcstate"]["media"], media)
        self.assertEqual(result["art_port"], 8751)


class MediaReaderTest(unittest.TestCase):
    reader = WindowsTest.reader

    def tearDown(self):
        pc_state.ART.set_current(None)
        pc_state._session_id = None

    def session(self, app, status):
        session = mock.Mock(source_app_user_model_id=app)
        session.get_playback_info.return_value = SimpleNamespace(playback_status=status)
        session.try_get_media_properties_async = mock.AsyncMock(
            return_value=SimpleNamespace(title="Song", artist="Artist", thumbnail=None))
        return session

    def test_session_choice_playing_current_first_none(self):
        chrome, tidal = self.session("chrome", 5), self.session("tidal", 4)
        manager = mock.Mock()
        manager.get_sessions.return_value = [chrome, tidal]
        manager.get_current_session.return_value = chrome
        self.assertIs(pc_state._choose_session(manager), tidal)
        tidal.get_playback_info.return_value.playback_status = 5
        self.assertIs(pc_state._choose_session(manager), chrome)
        self.assertIs(pc_state._choose_session(manager, keep="tidal"), tidal)
        manager.get_current_session.return_value = None
        self.assertIs(pc_state._choose_session(manager), chrome)
        manager.get_sessions.return_value = []
        self.assertIsNone(pc_state._choose_session(manager))

    def test_windows_estimated_position_and_selected_id(self):
        reader = self.reader()
        session = self.session("tidal", 4)
        reader._manager = mock.Mock()
        reader._manager.get_sessions.return_value = [session]
        reader._manager.get_current_session.return_value = session
        session.get_timeline_properties.return_value = SimpleNamespace(
            position=timedelta(seconds=0.0155), end_time=timedelta(seconds=180),
            last_updated_time=datetime.fromtimestamp(1700000000, timezone.utc))
        with mock.patch.object(pc_state.time, "time", return_value=1700000005.4):
            media = asyncio.run(reader._media())
        self.assertEqual((media["dur"], media["pos"], media["t0"]), (180, 5, 1700000000))
        self.assertEqual(pc_state.selected_session_id(), "tidal")
        session.get_playback_info.return_value.playback_status = 5
        with mock.patch.object(pc_state.time, "time", return_value=1700000090):
            media = asyncio.run(reader._media())
        self.assertEqual(media["pos"], 0)
        self.assertNotIn("t0", media)
        reader.close()

    def test_thumbnail_refresh_crc_change_clear_and_timeout(self):
        reader = self.reader()
        reader._media_key = ("chrome", "Song", "Artist")
        reader._props = SimpleNamespace(thumbnail=object())
        reader._thumbnail = mock.AsyncMock(return_value=b"png bytes")
        with mock.patch.object(pc_state.time, "monotonic", return_value=10) as clock:
            media = {}
            asyncio.run(reader._refresh_art(media))
            self.assertEqual(media["art"], "%08x" % zlib.crc32(b"png bytes"))
            clock.return_value = 14.9
            asyncio.run(reader._refresh_art({}))
            self.assertEqual(reader._thumbnail.await_count, 1)
            clock.return_value = 15
            reader._thumbnail.return_value = b"late chrome cover"
            updated = {}
            asyncio.run(reader._refresh_art(updated))
            self.assertNotEqual(media["art"], updated["art"])
            clock.return_value = 20
            reader._thumbnail.side_effect = asyncio.TimeoutError
            retained = {}
            asyncio.run(reader._refresh_art(retained))
            self.assertEqual(retained, updated)
            reader._media_key = ("chrome", "New song", "Artist")
            reader._thumbnail.side_effect = None
            reader._thumbnail.return_value = None
            cleared = {}
            asyncio.run(reader._refresh_art(cleared))
            self.assertNotIn("art", cleared)
            self.assertIsNone(pc_state.ART.pixels(updated["art"], 32))
        reader.close()

    def test_winrt_thumbnail_stream_buffer_and_short_read(self):
        reader = self.reader()
        stream = mock.Mock(size=5)
        stream.read_async = mock.AsyncMock(return_value=bytearray(b"cover"))
        thumbnail = SimpleNamespace(open_read_async=mock.AsyncMock(return_value=stream))
        reader._props = SimpleNamespace(thumbnail=thumbnail)
        streams = SimpleNamespace(Buffer=mock.Mock(side_effect=bytearray),
                                  InputStreamOptions=SimpleNamespace(READ_AHEAD=1))
        with mock.patch.dict(sys.modules, {"winrt.windows.storage.streams": streams}):
            self.assertEqual(asyncio.run(reader._thumbnail()), b"cover")
            streams.Buffer.assert_called_once_with(5)
            self.assertEqual(stream.read_async.call_args.args[1:], (5, 1))
            stream.close.assert_called_once()
            stream.read_async.return_value = bytearray(b"bad")
            self.assertIsNone(asyncio.run(reader._thumbnail()))
            self.assertEqual(stream.close.call_count, 2)
        reader.close()

    def test_thumbnail_budget_is_outside_media_timeout(self):
        reader = self.reader()
        reader._volume = lambda: {}
        reader._media_key = ("player", "Song", "Artist")
        reader._props = SimpleNamespace(thumbnail=object())
        reader._media = mock.AsyncMock(return_value={"status": "playing", "title": "Song"})

        async def thumbnail():
            await asyncio.sleep(0.45)
            return b"cover"

        reader._thumbnail = thumbnail
        state = asyncio.run(reader.read())
        self.assertEqual(state["media"]["title"], "Song")
        self.assertIn("art", state["media"])
        reader.close()

    def test_linux_file_art_only_and_playing_timeline(self):
        reader = pc_state._LinuxReader()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "a cover.png"
            path.write_bytes(b"cover")
            for url, has_art in ((path.as_uri(), True), ("https://example.com/cover", False)):
                output = [None, None, "Playing", "Artist\tSong\t180000000\t" + url, "12.5"]
                with mock.patch.object(reader, "_run", side_effect=output), \
                        mock.patch.object(pc_state.time, "time", return_value=1700000012.5), \
                        mock.patch.object(pc_state.Path, "read_bytes", return_value=b"cover") as read:
                    media = asyncio.run(reader.read())["media"]
                self.assertEqual((media["dur"], media["pos"], media["t0"]), (180, 12, 1700000000))
                self.assertEqual("art" in media, has_art)
                self.assertEqual(read.call_count, int(has_art))


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
