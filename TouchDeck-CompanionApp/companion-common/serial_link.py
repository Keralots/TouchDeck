"""Optional USB transport for TouchDeck metrics and authenticated commands."""

import json
import threading
import time

import serial
from serial.tools import list_ports

import pc_control


SCAN_INTERVAL = 5.0
HANDSHAKE_TIMEOUT = 2.0
REJECT_INTERVAL = 60.0
_link = None


def status():
    link = _link
    return link.status() if link is not None else {"connected": False, "port": ""}


def _frame(payload):
    return ("@TD " + json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")


class SerialLink:
    def __init__(self, state, stop_event=None):
        self._state = state
        self._app_stop = stop_event or threading.Event()
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._serial = None
        self._buffer = b""
        self._discard_line = False
        self._rejected = {}
        self._thread = threading.Thread(target=self._run, daemon=True, name="serial-link")

    def start(self):
        global _link
        _link = self
        self._thread.start()

    def _stopping(self):
        return self._stop.is_set() or self._app_stop.is_set()

    def status(self):
        with self._lock:
            return {"connected": self._serial is not None,
                    "port": str(self._serial.port) if self._serial is not None else ""}

    def is_connected(self):
        return self.status()["connected"]

    def port_name(self):
        return self.status()["port"]

    def _close_locked(self):
        port, self._serial = self._serial, None
        if port is not None:
            try:
                port.close()
            except OSError:
                pass

    def _disconnect(self):
        with self._lock:
            self._close_locked()

    def stop(self):
        self._stop.set()
        self._disconnect()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=3.0)

    def send(self, payload):
        data = _frame(payload)
        with self._lock:
            if self._serial is None or self._stopping():
                return False
            try:
                if self._serial.write(data) != len(data):
                    raise serial.SerialException("Incomplete serial write")
                return True
            except OSError:
                self._close_locked()
                return False

    def _read_packet(self, port):
        # A serial timeout can split a line anywhere, including inside UTF-8.
        if b"\n" not in self._buffer:
            self._buffer += port.read(min(port.in_waiting or 1, 4096))
        while b"\n" in self._buffer:
            line, self._buffer = self._buffer.split(b"\n", 1)
            if self._discard_line:
                self._discard_line = False
                continue
            if not line.startswith(b"@TD "):
                continue
            try:
                packet = json.loads(line[4:].decode("utf-8"))
            except (ValueError, UnicodeError):
                continue
            if isinstance(packet, dict):
                return packet
        # Bound memory when a noisy device never terminates its log line.
        if len(self._buffer) > 65536:
            self._buffer = b""
            self._discard_line = True
        return None

    def _setting(self):
        return self._state.get_config().get("serial_port", "auto")

    def _candidates(self, setting):
        if setting == "off":
            return []
        if setting != "auto":
            return [setting]
        return [port.device for port in list_ports.comports()
                if port.vid == 0x303A and port.pid == 0x1001]

    def _connect(self, name, setting):
        port = None
        accepted = False
        try:
            port = serial.Serial(baudrate=115200, timeout=0.2, write_timeout=0.2,
                                 dsrdtr=False, rtscts=False)
            port.port = name
            # Set the inactive levels before open to avoid resetting the ESP32.
            port.dtr = False
            port.rts = False
            port.open()
            self._buffer = b""
            self._discard_line = False
            hello = _frame({"hello": 1})
            if port.write(hello) != len(hello):
                raise serial.SerialException("Incomplete handshake write")
            deadline = time.monotonic() + HANDSHAKE_TIMEOUT
            while not self._stopping() and self._setting() == setting:
                if time.monotonic() >= deadline:
                    self._rejected[name] = time.monotonic() + REJECT_INTERVAL
                    break
                packet = self._read_packet(port)
                if packet is not None and packet.get("hello") == "TouchDeck":
                    with self._lock:
                        if not self._stopping() and self._setting() == setting:
                            self._serial = port
                            accepted = True
                    break
        except (OSError, ValueError):
            pass
        finally:
            if port is not None and not accepted:
                try:
                    port.close()
                except OSError:
                    pass
        return accepted

    def _dispatch(self, packet):
        if "cmd" not in packet:
            return
        try:
            ok, why = pc_control.verify(self._state.get_config(), packet)
            if ok:
                ok, why = pc_control.execute(packet["cmd"])
        except (TypeError, ValueError):
            ok, why = False, "invalid command packet"
        self.send({"ack": packet.get("nonce", ""), "ok": ok, "msg": why})

    def _run(self):
        setting = None
        next_scan = 0.0
        try:
            while not self._stopping():
                current = self._setting()
                if current != setting:
                    self._disconnect()
                    setting = current
                    next_scan = 0.0
                with self._lock:
                    port = self._serial
                if port is not None:
                    try:
                        packet = self._read_packet(port)
                        if packet is not None and self.is_connected():
                            self._dispatch(packet)
                    except (OSError, TypeError):
                        self._disconnect()
                    continue
                now = time.monotonic()
                if setting != "off" and now >= next_scan:
                    try:
                        for name in self._candidates(setting):
                            if self._stopping() or self._setting() != setting:
                                break
                            if self._rejected.get(name, 0) > time.monotonic():
                                continue
                            if self._connect(name, setting):
                                break
                    except OSError:
                        pass
                    next_scan = time.monotonic() + SCAN_INTERVAL
                self._stop.wait(0.2)
        finally:
            self._disconnect()
