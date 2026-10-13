"""Cached volume and media state for PC command tiles."""

import asyncio
import copy
import json
import logging
import os
import re
import subprocess
import sys
import threading
import time
import unicodedata

POLL_INTERVAL = 0.5
COMMAND_DELAY = 0.2
TOOL_TIMEOUT = 0.2
PACKET_LIMIT = 1900
_log = logging.getLogger(__name__)


def _text(value):
    return "".join(c for c in str(value)
                   if not unicodedata.category(c).startswith("C")).strip()[:48]


def _clean(state):
    result = {}
    if state.get("vol") is not None:
        result["vol"] = max(0, min(100, round(state["vol"])))
    if state.get("mute") is not None:
        result["mute"] = bool(state["mute"])
    media = state.get("media")
    if media is not None:
        clean = {}
        if media.get("status") in ("playing", "paused", "stopped"):
            clean["status"] = media["status"]
        for key in ("title", "artist"):
            if media.get(key) is not None:
                clean[key] = _text(media[key])
        if clean:
            result["media"] = clean
    return result


def encode_packet(payload):
    """Return a copy and its UDP encoding; the PC state gives way to the size limit."""
    payload = copy.deepcopy(payload)

    def encoded():
        return json.dumps(payload).encode("utf-8")

    data = encoded()
    media = payload.get("pcstate", {}).get("media")
    if len(data) >= PACKET_LIMIT and media is not None:
        media.pop("title", None)
        media.pop("artist", None)
        data = encoded()
        if len(data) >= PACKET_LIMIT:
            payload["pcstate"].pop("media", None)
            data = encoded()
    if len(data) >= PACKET_LIMIT and "pcstate" in payload:
        payload.pop("pcstate")
        data = encoded()
    return payload, data


class _LinuxReader:
    def __init__(self):
        self._missing = set()

    def _run(self, *args):
        if args[0] in self._missing:
            return None
        try:
            return subprocess.run(args, capture_output=True, text=True,
                                  timeout=TOOL_TIMEOUT, check=True,
                                  env=dict(os.environ, LC_ALL="C")).stdout
        except FileNotFoundError:
            self._missing.add(args[0])
            _log.warning("pc_state: optional tool %s is unavailable", args[0])
        except (OSError, subprocess.SubprocessError):
            pass
        return None

    async def read(self):
        state = {}
        volume = self._run("pactl", "get-sink-volume", "@DEFAULT_SINK@")
        if volume is not None:
            match = re.search(r"(\d+)%", volume)
            if match:
                state["vol"] = int(match[1])
        mute = self._run("pactl", "get-sink-mute", "@DEFAULT_SINK@")
        if mute is not None:
            match = re.search(r"Mute:\s*(yes|no)\b", mute, re.IGNORECASE)
            if match:
                state["mute"] = match[1].lower() == "yes"
        status = self._run("playerctl", "status")
        if status is not None and status.strip().lower() in ("playing", "paused", "stopped"):
            media = {"status": status.strip().lower()}
            metadata = self._run("playerctl", "metadata", "--format", "{{artist}}\t{{title}}")
            if metadata is not None and "\t" in metadata:
                media["artist"], media["title"] = metadata.rstrip("\r\n").split("\t", 1)
            state["media"] = media
        return state


class _WindowsReader:
    def __init__(self):
        self._com = None
        self._audio = None
        self._manager_type = None
        self._manager = None
        self._endpoint = None
        self._device_id = None
        try:
            import comtypes
            comtypes.CoInitialize()
            self._com = comtypes
            from pycaw.pycaw import AudioUtilities
            self._audio = AudioUtilities
        except Exception as exc:
            _log.warning("pc_state: optional pycaw/comtypes unavailable: %s", exc)
        try:
            from winrt.windows.media.control import GlobalSystemMediaTransportControlsSessionManager
            self._manager_type = GlobalSystemMediaTransportControlsSessionManager
        except Exception as exc:
            _log.warning("pc_state: optional winrt media control unavailable: %s", exc)

    def _volume(self):
        if self._audio is None:
            return {}
        try:
            device = self._audio.GetSpeakers()
            if self._endpoint is None or device.id != self._device_id:
                self._endpoint = device.EndpointVolume
                self._device_id = device.id
            return {"vol": self._endpoint.GetMasterVolumeLevelScalar() * 100,
                    "mute": bool(self._endpoint.GetMute())}
        except Exception:
            self._endpoint = None
            self._device_id = None
            return {}

    async def _media(self):
        if self._manager is None:
            self._manager = await self._manager_type.request_async()
        session = self._manager.get_current_session()
        if session is None:
            return None
        media = {}
        info = session.get_playback_info()
        if info is not None:
            status = {3: "stopped", 4: "playing", 5: "paused"}.get(info.playback_status)
            if status is not None:
                media["status"] = status
        try:
            props = await asyncio.wait_for(session.try_get_media_properties_async(), timeout=0.3)
            media.update(title=props.title, artist=props.artist)
        except Exception:
            pass
        return media

    async def read(self):
        state = self._volume()
        if self._manager_type is not None:
            try:
                state["media"] = await asyncio.wait_for(self._media(), timeout=0.4)
            except Exception:
                self._manager = None
        return state

    def close(self):
        self._endpoint = self._manager = None
        if self._com is not None:
            self._com.CoUninitialize()


class PCState:
    def __init__(self, reader_factory=None):
        self.changed = threading.Event()
        self._condition = threading.Condition()
        self._state = {}
        self._stop = False
        self._early = None
        self._thread = None
        self._reader_factory = reader_factory or (
            _WindowsReader if sys.platform == "win32" else _LinuxReader)

    def start(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._run, daemon=True, name="pc-state")
            self._thread.start()

    def stop(self):
        with self._condition:
            self._stop = True
            self._condition.notify_all()
        self.changed.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def read(self):
        with self._condition:
            return copy.deepcopy(self._state)

    def request_poll(self):
        with self._condition:
            due = time.monotonic() + COMMAND_DELAY
            self._early = min(self._early, due) if self._early is not None else due
            self._condition.notify_all()

    def _publish(self, state):
        state = _clean(state)
        with self._condition:
            if state != self._state:
                self._state = state
                self.changed.set()

    def _run(self):
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        reader = self._reader_factory()
        due = time.monotonic()
        try:
            while True:
                with self._condition:
                    if self._stop:
                        break
                    now = time.monotonic()
                    next_poll = min(due, self._early) if self._early is not None else due
                    if now < next_poll:
                        self._condition.wait(next_poll - now)
                        continue
                    if self._early is not None and now >= self._early:
                        self._early = None
                try:
                    self._publish(loop.run_until_complete(reader.read()))
                except Exception:
                    self._publish({})
                due = max(now + POLL_INTERVAL, time.monotonic() + 0.01)
        finally:
            if hasattr(reader, "close"):
                reader.close()
            loop.close()
            asyncio.set_event_loop(None)


STATE = PCState()
