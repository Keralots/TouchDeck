"""Audio spectrum streamer for the deck's visualizer tiles.

Captures what the PC is playing (WASAPI loopback on Windows, PulseAudio
monitor on Linux, both via the `soundcard` package), reduces each window to
32 log-spaced bands plus a trigger-aligned waveform, and sends them as a
binary UDP packet: b"FFT1" + 32 band bytes + 128 waveform bytes, 50 per
second, to the same port the stats JSON uses. Tiles show bars or a scope.

Optional: without `soundcard` and `numpy` AVAILABLE is False and ensure() is
a no-op. Managed through ensure(config), called at startup and after the
audio switch changes.
"""

import socket
import sys
import threading
import time
from contextlib import contextmanager

try:
    import numpy as np
    import soundcard as sc
    AVAILABLE = True
    _IMPORT_ERROR = ""
except Exception as e:  # missing package or no audio backend
    np = None
    sc = None
    AVAILABLE = False
    _IMPORT_ERROR = str(e)

RATE = 48000
FRAMES = 1920            # 40 ms analysis window
HOP = 960                # a new overlapping window every 20 ms -> 50 packets/s
FFT_N = 2048
BANDS = 32
F_LO, F_HI = 50.0, 16000.0

# Decimating by 8 low-passes the trace to ~3 kHz, so it reads as a scope.
WAVE_POINTS = 128
WAVE_DECIM = 8
WAVE_AGC_DECAY = 0.55    # per second, multiplicative
WAVE_AGC_FLOOR = 0.02    # silence stays flat instead of amplified noise
WAVE_GAIN = 118.0        # peak deflection inside +/-128

# Band AGC: the reference tracks the loudest recent band and decays slowly.
AGC_RANGE_DB = 38.0
AGC_DECAY_DB_PER_S = 1.5
AGC_FLOOR_DB = -55.0
SILENT_DB = -120.0

# Packets go out from the capture thread (Windows timers round to ~15.6 ms and
# would fall behind). A watchdog only fills capture stalls: it repeats, then
# fades, the last frame, then stops so the deck shows "No audio".
GAP_S = 0.12
HOLD_S = 0.5
STALL_DECAY = 0.85
GIVE_UP_S = 6.0
RETRY_WAITS = (0.25, 0.5, 1.0, 3.0)

# A loopback client that outlives a suspend or an audio engine restart reads
# zeros forever without raising, so a run of silence reopens it, backing off.
SILENCE_REOPEN_S = 10.0
SILENCE_REOPEN_MAX_S = 60.0
RESUME_JUMP_S = 5.0      # wall clock jump across one block: we just resumed


def udp_target(address, port):
    """Host part of the device address (it may carry an HTTP port)."""
    host = (address or "").strip()
    if host.count(":") == 1:
        host = host.split(":", 1)[0]
    return host, int(port or 4210)


def frame_action(age_s):
    if age_s <= HOLD_S:
        return "hold"
    if age_s < GIVE_UP_S:
        return "decay"
    return "stop"


def boost_thread_priority():
    """Windows: MMCSS "Pro Audio" plus HIGHEST priority, so a busy CPU does not
    starve capture. Keep the returned handle for the thread's lifetime."""
    handle = None
    try:
        import ctypes
        from ctypes import wintypes
        avrt = ctypes.WinDLL("avrt")
        avrt.AvSetMmThreadCharacteristicsW.restype = wintypes.HANDLE
        avrt.AvSetMmThreadCharacteristicsW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
        task_index = wintypes.DWORD(0)
        handle = avrt.AvSetMmThreadCharacteristicsW("Pro Audio", ctypes.byref(task_index)) or None
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentThread.restype = wintypes.HANDLE  # 64-bit pseudo-handle overflows an int
        kernel32.SetThreadPriority.argtypes = [wintypes.HANDLE, ctypes.c_int]
        kernel32.SetThreadPriority(kernel32.GetCurrentThread(), 2)
    except Exception:
        pass
    return handle


def release_thread_priority(handle):
    if handle:
        import ctypes
        from ctypes import wintypes
        avrt = ctypes.WinDLL("avrt")
        avrt.AvRevertMmThreadCharacteristics.argtypes = [wintypes.HANDLE]
        avrt.AvRevertMmThreadCharacteristics.restype = wintypes.BOOL
        avrt.AvRevertMmThreadCharacteristics(handle)


@contextmanager
def audio_thread_com():
    """COM belongs to the calling thread, not the SoundCard import thread."""
    ole32 = None
    initialized = False
    if sys.platform == "win32":
        import ctypes
        ole32 = ctypes.OleDLL("ole32")
        ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        ole32.CoInitializeEx.restype = ctypes.c_long
        ole32.CoUninitialize.argtypes = []
        ole32.CoUninitialize.restype = None
        try:
            result = ole32.CoInitializeEx(None, 0)  # COINIT_MULTITHREADED
            initialized = result in (0, 1)
        except OSError as error:
            if (error.winerror & 0xffffffff) != 0x80010106:
                raise  # anything but "already initialized in another apartment"
    try:
        yield
    finally:
        if initialized:
            ole32.CoUninitialize()


class SpectrumStreamer(threading.Thread):
    def __init__(self, hosts, port):
        super().__init__(daemon=True, name="audio-spectrum")
        self._lock = threading.Lock()
        self._requested = (tuple(hosts), int(port))
        self._targets = {}  # host -> address, resolved off the capture thread
        self._default_device_id = None
        self._stop_event = threading.Event()
        self._wake = threading.Event()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.last_sent = 0.0
        self.last_error = ""
        self.last_level_db = SILENT_DB
        self.stalls = 0
        self.reopens = 0
        self._last_bands = None
        self._last_wave = None
        self._last_frame_at = 0.0

        self._window = np.hanning(FRAMES).astype(np.float32)
        edges = F_LO * (F_HI / F_LO) ** (np.arange(BANDS + 1) / BANDS)
        bin_hz = RATE / float(FFT_N)
        self._band_bins = []
        for i in range(BANDS):
            lo = int(edges[i] / bin_hz)
            self._band_bins.append((lo, max(lo + 1, int(edges[i + 1] / bin_hz))))
        self._agc_ref = AGC_FLOOR_DB
        self._wave_ref = WAVE_AGC_FLOOR
        self._analysis = np.zeros(FRAMES, dtype=np.float32)

    def set_targets(self, hosts, port):
        with self._lock:
            if (tuple(hosts), int(port)) == self._requested:
                return
            self._requested = (tuple(hosts), int(port))
            self._targets = {}
        self._wake.set()

    def stop(self):
        self._stop_event.set()
        self._wake.set()

    def _bands(self, mono):
        spec = np.abs(np.fft.rfft(mono * self._window, n=FFT_N))
        amps = np.array([np.sqrt(np.mean(spec[lo:hi] ** 2)) for lo, hi in self._band_bins], dtype=np.float32)
        db = 20.0 * np.log10(amps + 1e-7)
        self._agc_ref = max(self._agc_ref - AGC_DECAY_DB_PER_S * HOP / float(RATE), float(db.max()), AGC_FLOOR_DB)
        norm = (db - (self._agc_ref - AGC_RANGE_DB)) / AGC_RANGE_DB
        return np.clip(norm * 255.0, 0, 255).astype(np.uint8)

    def _wave(self, mono):
        usable = (len(mono) // WAVE_DECIM) * WAVE_DECIM
        low = mono[:usable].reshape(-1, WAVE_DECIM).mean(axis=1)
        if len(low) < WAVE_POINTS:
            low = np.pad(low, (0, WAVE_POINTS - len(low)))
        # Start on the newest rising zero crossing so the trace stands still.
        start = 0
        limit = len(low) - WAVE_POINTS
        if limit > 0:
            head = low[:limit + 1]
            rising = np.nonzero((head[:-1] <= 0.0) & (head[1:] > 0.0))[0]
            if rising.size:
                start = int(rising[-1]) + 1
        seg = low[start:start + WAVE_POINTS]
        self._wave_ref = max(self._wave_ref * (WAVE_AGC_DECAY ** (HOP / float(RATE))),
                             float(np.abs(seg).max()), WAVE_AGC_FLOOR)
        return np.clip(seg / self._wave_ref * WAVE_GAIN + 128.0, 0, 255).astype(np.uint8)

    def _send(self, bands, wave):
        with self._lock:
            targets = list(self._targets.values())
        if not targets or self._stop_event.is_set():
            return
        packet = b"FFT1" + bands.astype(np.uint8).tobytes() + wave.astype(np.uint8).tobytes()
        for target in targets:
            try:
                self._sock.sendto(packet, target)
                self.last_sent = time.monotonic()
            except OSError:
                pass

    def _watchdog(self):
        mmcss = boost_thread_priority()
        try:
            holding = False
            while not self._stop_event.is_set():
                self._stop_event.wait(GAP_S / 2.0)
                now = time.monotonic()
                with self._lock:
                    bands, wave, frame_at = self._last_bands, self._last_wave, self._last_frame_at
                if bands is None or not self.last_sent or now - self.last_sent < GAP_S:
                    holding = False
                    continue
                action = frame_action(now - frame_at)
                if action == "stop":
                    continue
                if not holding:
                    holding = True
                    self.stalls += 1
                if action == "decay":
                    bands = bands * STALL_DECAY
                    wave = np.clip((wave.astype(np.float32) - 128.0) * STALL_DECAY + 128.0, 0, 255).astype(np.uint8)
                    with self._lock:
                        self._last_bands, self._last_wave = bands, wave
                self._send(bands, wave)
        finally:
            release_thread_priority(mmcss)

    def _maintenance(self):
        """DNS and device enumeration never run between captured blocks."""
        resolved_for = None
        resolve_at = check_at = 0.0
        with audio_thread_com():
            while not self._stop_event.is_set():
                now = time.monotonic()
                with self._lock:
                    requested = self._requested
                if requested != resolved_for or now >= resolve_at:
                    hosts, port = requested
                    resolve_at = now + 30.0
                    for host in hosts:
                        try:
                            address = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)[0][4]
                            with self._lock:
                                if requested == self._requested:
                                    self._targets[host] = address
                        except OSError:
                            resolve_at = now + 2.0  # keep the last good address meanwhile
                    resolved_for = requested
                if now >= check_at:
                    try:
                        device_id = sc.default_speaker().id
                        with self._lock:
                            self._default_device_id = device_id
                    except Exception:
                        pass
                    check_at = now + 10.0
                self._wake.wait(1.0)
                self._wake.clear()

    def run(self):
        mmcss = boost_thread_priority()
        watchdog = threading.Thread(target=self._watchdog, daemon=True, name="audio-watchdog")
        maintenance = threading.Thread(target=self._maintenance, daemon=True, name="audio-device-control")
        try:
            with audio_thread_com():
                watchdog.start()
                maintenance.start()
                self._capture_loop()
        except Exception as error:
            self.last_error = str(error)
        finally:
            self.stop()
            if watchdog.ident is not None:
                watchdog.join(timeout=1.0)
            if maintenance.ident is not None:
                maintenance.join(timeout=5.0)
            self._sock.close()
            release_thread_priority(mmcss)

    def _capture_loop(self):
        attempt = 0
        reopen_after = SILENCE_REOPEN_S
        while not self._stop_event.is_set():
            try:
                # Re-resolve the default output each open: a headphone switch
                # is picked up on the next reconnect.
                spk = sc.default_speaker()
                with self._lock:
                    self._default_device_id = spk.id
                mic = sc.get_microphone(id=spk.id, include_loopback=True)
                with mic.recorder(samplerate=RATE, blocksize=HOP) as rec:
                    attempt = 0
                    silent_since = None
                    last_wall = time.time()
                    while not self._stop_event.is_set():
                        data = rec.record(numframes=HOP)
                        now_wall = time.time()
                        wall_gap = now_wall - last_wall
                        last_wall = now_wall
                        mono = data.mean(axis=1) if data.ndim > 1 else data
                        self._analysis = np.concatenate((self._analysis, mono.astype(np.float32)))[-FRAMES:]
                        bands = self._bands(self._analysis)
                        wave = self._wave(self._analysis)
                        rms = float(np.sqrt(np.mean(self._analysis ** 2)))
                        level = max(SILENT_DB, 20.0 * np.log10(rms)) if rms > 0 else SILENT_DB
                        self.last_level_db = level
                        self.last_error = ""
                        now = time.monotonic()
                        with self._lock:
                            self._last_bands = bands.astype(np.float32)
                            self._last_wave = wave
                            self._last_frame_at = now
                            default_id = self._default_device_id
                        self._send(bands, wave)
                        if level > SILENT_DB:
                            silent_since = None
                            reopen_after = SILENCE_REOPEN_S
                        elif silent_since is None:
                            silent_since = now
                        silence = 0.0 if silent_since is None else now - silent_since
                        if wall_gap >= RESUME_JUMP_S or silence >= reopen_after:
                            reopen_after = min(reopen_after * 2, SILENCE_REOPEN_MAX_S)
                            self.reopens += 1
                            break
                        if default_id is not None and default_id != spk.id:
                            break  # reopen on the new output
            except Exception as e:
                self.last_error = str(e)
                self._stop_event.wait(RETRY_WAITS[min(attempt, len(RETRY_WAITS) - 1)])
                attempt += 1


_lock = threading.Lock()
_streamer = None


def ensure(config):
    """Start, stop or redirect the streamer to match the config. Safe to call often."""
    global _streamer
    if not AVAILABLE:
        return
    config = config or {}
    # The deck plus the extra panels, as for the metrics packets.
    addresses = [config.get("esp32_ip")] + [p.get("address") for p in config.get("panels", [])]
    hosts, port = [], 4210
    for address in addresses:
        host, port = udp_target(address, config.get("udp_port"))
        if host and host not in hosts:
            hosts.append(host)
    want = bool(config.get("audio_viz")) and bool(hosts)
    with _lock:
        if _streamer is not None and not _streamer.is_alive():
            _streamer = None
        if want and _streamer is None:
            _streamer = SpectrumStreamer(hosts, port)
            _streamer.start()
        elif want:
            _streamer.set_targets(hosts, port)
        elif _streamer is not None:
            _streamer.stop()
            _streamer.join(timeout=2.0)
            if not _streamer.is_alive():
                _streamer = None


def stop_all():
    global _streamer
    with _lock:
        if _streamer is not None:
            _streamer.stop()
            _streamer.join(timeout=2.0)
            if not _streamer.is_alive():
                _streamer = None


def status():
    """Fields merged into /api/status for the web UI."""
    with _lock:
        s = _streamer
        running = s is not None and s.is_alive()
        return {
            "audioAvailable": AVAILABLE,
            "audioReason": _IMPORT_ERROR,
            "audioRunning": running,
            "audioSending": running and time.monotonic() - s.last_sent < 2.0,
            "audioError": s.last_error if s is not None else "",
            "audioLevel": round(float(s.last_level_db), 1) if running else SILENT_DB,
            "audioStalls": s.stalls if s is not None else 0,
            "audioReopens": s.reopens if s is not None else 0,
        }
