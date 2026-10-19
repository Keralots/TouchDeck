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
from pathlib import Path
from urllib.parse import unquote, urlsplit

from art_server import ART

POLL_INTERVAL = 0.5
COMMAND_DELAY = 0.2
TOOL_TIMEOUT = 0.2
# arduino-esp32 NetworkUdp reads at most 1460 bytes; a longer datagram is truncated.
PACKET_LIMIT = 1460
_log = logging.getLogger(__name__)
_session_id = None


def selected_session_id():
    return _session_id


_winrt_busy = {}


def run_winrt(key, make_coro, timeout):
    """Run a WinRT coroutine on a fresh thread; None on timeout, error or a stuck earlier call.

    In a COM STA thread (comtypes.CoInitialize for pycaw) WinRT async calls never complete.
    """
    if _winrt_busy.get(key) is not None and _winrt_busy[key].is_alive():
        return None
    result = []

    def run():
        try:
            result.append(asyncio.run(make_coro()))
        except Exception:
            pass

    worker = threading.Thread(target=run, daemon=True, name="winrt-" + key)
    _winrt_busy[key] = worker
    worker.start()
    worker.join(timeout)
    return result[0] if result else None


def _choose_session(manager, app_id=None, keep=None):
    sessions = [session for session in manager.get_sessions()
                if app_id is None or session.source_app_user_model_id == app_id]
    for session in sessions:
        try:
            if session.get_playback_info().playback_status == 4:
                return session
        except Exception:
            pass
    # Nothing plays: stay on the session shown before, so pausing it does not switch apps.
    for session in sessions:
        if keep is not None and session.source_app_user_model_id == keep:
            return session
    current = manager.get_current_session()
    if current is not None and app_id is not None and current.source_app_user_model_id != app_id:
        current = None
    return current or (sessions[0] if sessions else None)


class _Timeline:
    def __init__(self):
        self._key = None
        self._last = {}

    def apply(self, media, key, duration, position, now):
        if duration > 0:
            media["dur"] = int(duration)
        position = max(0, min(position, duration) if duration > 0 else position)
        media["pos"] = int(position)
        if media.get("status") == "playing":
            media["t0"] = round(now - position)
            if (key == self._key and self._last.get("status") == "playing"
                    and abs(media["t0"] - self._last["t0"]) <= 1):
                media["t0"] = self._last["t0"]
                media["pos"] = self._last["pos"]
        self._key, self._last = key, dict(media)


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
        for key in ("dur", "pos", "t0"):
            value = media.get(key)
            if isinstance(value, int) and not isinstance(value, bool):
                if 0 <= value <= 2147483647 and (key == "pos" or value > 0):
                    if key != "t0" or clean.get("status") == "playing":
                        clean[key] = value
        art = media.get("art")
        if isinstance(art, str) and re.fullmatch(r"[0-9a-fA-F]{8}", art):
            clean["art"] = art.lower()
        if clean:
            result["media"] = clean
    return result


def encode_packet(payload):
    """Return a copy and its UDP encoding; the PC state gives way to the size limit."""
    payload = copy.deepcopy(payload)

    def encoded():
        return json.dumps(payload, separators=(",", ":")).encode("utf-8")

    data = encoded()
    media = payload.get("pcstate", {}).get("media")
    if len(data) > PACKET_LIMIT and media is not None:
        media.pop("title", None)
        media.pop("artist", None)
        data = encoded()
        if len(data) > PACKET_LIMIT:
            payload["pcstate"].pop("media", None)
            data = encoded()
    if len(data) > PACKET_LIMIT and "pcstate" in payload:
        payload.pop("pcstate")
        data = encoded()
    return payload, data


class _LinuxReader:
    def __init__(self):
        self._missing = set()
        self._timeline = _Timeline()

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
            metadata = self._run("playerctl", "metadata", "--format",
                                 "{{artist}}\t{{title}}\t{{mpris:length}}\t{{mpris:artUrl}}")
            fields = metadata.rstrip("\r\n").split("\t") if metadata else []
            if len(fields) >= 2:
                media["artist"], media["title"] = fields[:2]
            position = self._run("playerctl", "position")
            try:
                duration = float(fields[2]) / 1000000 if len(fields) > 2 and fields[2] else 0
                self._timeline.apply(media, tuple(fields[:2]), duration, float(position), time.time())
            except (TypeError, ValueError):
                pass
            source = None
            if len(fields) > 3:
                url = urlsplit(fields[3])
                if url.scheme == "file" and url.netloc in ("", "localhost"):
                    try:
                        source = Path(unquote(url.path)).read_bytes()
                    except OSError:
                        pass
            art = ART.set_current(source)
            if art:
                media["art"] = art
            state["media"] = media
        else:
            ART.set_current(None)
        return state


class _WindowsReader:
    def __init__(self):
        self._com = None
        self._audio = None
        self._manager_type = None
        self._manager = None
        self._enumerator = None
        self._endpoint = None
        self._device_id = None
        self._timeline = _Timeline()
        self._art_key = None
        self._art_at = 0
        self._art = None
        self._props = None
        self._media_key = None
        try:
            import comtypes
            comtypes.CoInitialize()
            self._com = comtypes
            from pycaw import constants, pycaw
            self._audio = pycaw
            self._enumerator_clsid = constants.CLSID_MMDeviceEnumerator
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
        audio, com = self._audio, self._com
        try:
            # Not AudioUtilities.GetSpeakers(): it reads the whole device property
            # store on every call, which made this poll the companion's main CPU cost.
            if self._enumerator is None:
                self._enumerator = com.CoCreateInstance(
                    self._enumerator_clsid, audio.IMMDeviceEnumerator, com.CLSCTX_INPROC_SERVER)
            device = self._enumerator.GetDefaultAudioEndpoint(
                audio.EDataFlow.eRender.value, audio.ERole.eMultimedia.value)
            device_id = device.GetId()
            if self._endpoint is None or device_id != self._device_id:
                self._endpoint = device.Activate(
                    audio.IAudioEndpointVolume._iid_, com.CLSCTX_ALL, None
                ).QueryInterface(audio.IAudioEndpointVolume)
                self._device_id = device_id
            return {"vol": self._endpoint.GetMasterVolumeLevelScalar() * 100,
                    "mute": bool(self._endpoint.GetMute())}
        except Exception:
            self._enumerator = None
            self._endpoint = None
            self._device_id = None
            return {}

    async def _media(self):
        if self._manager is None:
            self._manager = await self._manager_type.request_async()
        global _session_id
        session = _choose_session(self._manager, keep=_session_id)
        _session_id = session.source_app_user_model_id if session else None
        self._props = None
        if session is None:
            self._art = ART.set_current(None)
            self._art_key = None
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
            self._props = props
        except Exception:
            pass
        self._media_key = (_session_id, media.get("title"), media.get("artist"))
        try:
            timeline = session.get_timeline_properties()
            now = time.time()
            position = timeline.position.total_seconds()
            if media.get("status") == "playing":
                position += now - timeline.last_updated_time.timestamp()
            self._timeline.apply(media, self._media_key, timeline.end_time.total_seconds(),
                                 position, now)
        except Exception:
            pass
        return media

    async def _thumbnail(self):
        thumbnail = getattr(self._props, "thumbnail", None)
        if thumbnail is None:
            return None
        from winrt.windows.storage.streams import Buffer, InputStreamOptions
        stream = await thumbnail.open_read_async()
        try:
            size = stream.size
            if not 0 < size <= 8 * 1024 * 1024:
                return None
            buf = await stream.read_async(Buffer(size), size, InputStreamOptions.READ_AHEAD)
            data = bytes(buf)
            return data if len(data) == size else None
        finally:
            stream.close()

    async def _refresh_art(self, media):
        if self._media_key != self._art_key:
            self._art = ART.set_current(None)
            self._art_at = 0
            self._art_key = self._media_key
        if self._props is not None and (not self._art_at or time.monotonic() - self._art_at >= 5):
            self._art_at = time.monotonic()
            source = run_winrt("cover", self._thumbnail, 1.0)
            if source is not None:
                self._art = ART.set_current(source)
        if self._art:
            media["art"] = self._art

    async def read(self):
        state = self._volume()
        if self._manager_type is not None:
            try:
                state["media"] = await asyncio.wait_for(self._media(), timeout=0.4)
            except Exception:
                self._manager = None
                global _session_id
                _session_id = None
            # Thumbnail I/O has its own budget, so it cannot discard media state.
            if state.get("media") is not None:
                await self._refresh_art(state["media"])
        return state

    def close(self):
        self._enumerator = self._endpoint = self._manager = None
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
