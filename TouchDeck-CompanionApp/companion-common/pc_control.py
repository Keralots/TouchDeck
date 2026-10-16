"""PC control: commands from a paired TouchDeck.

The device sends {"pc", "cmd", "nonce", "sig"} over UDP to cmd_port, where sig
is hex HMAC-SHA256(key, "pc_id|nonce|cmd") and the nonce is one of the four
this companion put in a recent metrics packet. Each works once and for 30 s, so a
captured command cannot be replayed. The answer {"ack": nonce, "ok", "msg"}
goes back to the sender.

Pairing: the companion asks the device to show a code (/api/pc/pair), the user
types it here, and the device hands over the key once (/api/pc/pair/confirm).

Set TOUCHDECK_DRY_RUN=1 to log commands instead of running them.
"""

import ctypes
import hashlib
import hmac
import json
import os
import secrets
import socket
import subprocess
import sys
import threading
import time

import pc_state
from config_model import device_host

DEFAULT_CMD_PORT = 4211
NONCE_TTL = 30.0
COMMANDS = ("media_play_pause", "media_next", "media_prev", "media_stop",
            "volume_up", "volume_down", "volume_mute", "lock", "sleep",
            "restart", "shutdown")
# These would cut the acknowledgement off, so they start a moment after it.
DEFERRED = ("lock", "sleep", "restart", "shutdown")

# "hotkey:ctrl+shift+m": keys pressed in order, released in reverse.
HOTKEY_PREFIX = "hotkey:"
HOTKEY_MAX_KEYS = 5
_KEY_NAMES = (["ctrl", "shift", "alt", "win", "space", "enter", "tab", "esc", "backspace",
               "delete", "insert", "home", "end", "pageup", "pagedown", "up", "down", "left",
               "right", "printscreen"]
              + [chr(c) for c in range(ord("a"), ord("z") + 1)]
              + [str(d) for d in range(10)]
              + ["f%d" % n for n in range(1, 25)])


def parse_hotkey(cmd):
    """Key names of a "hotkey:..." command, or None when it is not a valid one."""
    if not isinstance(cmd, str) or not cmd.startswith(HOTKEY_PREFIX):
        return None
    keys = cmd[len(HOTKEY_PREFIX):].split("+")
    if (not 0 < len(keys) <= HOTKEY_MAX_KEYS or len(set(keys)) != len(keys)
            or any(k not in _KEY_NAMES for k in keys)):
        return None
    return keys


class NonceBook:
    """One-time nonces handed to the device in metrics packets."""

    def __init__(self):
        self._lock = threading.Lock()
        self._live = {}

    def issue(self):
        nonce = secrets.token_hex(8)
        now = time.time()
        with self._lock:
            self._live = {n: t for n, t in self._live.items() if now - t < NONCE_TTL}
            while len(self._live) >= 256:  # a very short interval: drop the oldest
                self._live.pop(next(iter(self._live)))
            self._live[nonce] = now
        return nonce

    def consume(self, nonce):
        with self._lock:
            issued = self._live.pop(nonce, None)
        return issued is not None and time.time() - issued < NONCE_TTL


NONCES = NonceBook()


def sign(key_hex, pc_id, nonce, cmd):
    msg = ("%s|%s|%s" % (pc_id, nonce, cmd)).encode("utf-8")
    return hmac.new(bytes.fromhex(key_hex), msg, hashlib.sha256).hexdigest()


def verify(config, packet, addr=None, *, via_usb=False):
    """Return (ok, reason) for a command packet."""
    if not isinstance(config, dict) or not isinstance(packet, dict):
        return False, "invalid command packet"
    pc, cmd, nonce, sig = (packet.get(field) for field in ("pc", "cmd", "nonce", "sig"))
    if not all(isinstance(value, str) for value in (pc, cmd, nonce)):
        return False, "invalid command packet"
    if (not isinstance(sig, str) or len(sig) != 64
            or any(ch not in "0123456789abcdefABCDEF" for ch in sig)):
        return False, "bad signature"
    # Only the USB dispatcher may omit the sender IP and use the deck's key.
    key = config.get("pc_key") or ""
    if not via_usb:
        if not isinstance(addr, (tuple, list)) or not addr or not isinstance(addr[0], str):
            return False, "unknown device"
        devices = [(config.get("esp32_ip", ""), key)]
        devices.extend((panel["address"], panel.get("key", ""))
                       for panel in config.get("panels", []))
        resolved = {}
        for address, device_key in devices:
            try:
                host = device_host(address)
                if host not in resolved:
                    try:
                        resolved[host] = socket.gethostbyname(host)
                    except OSError:
                        resolved[host] = None
                if resolved[host] == addr[0]:
                    key = device_key
                    break
            except ValueError:
                continue
        else:
            return False, "unknown device"
    if not key:
        return False, "this PC is not paired"
    if pc != config.get("pc_id"):
        return False, "command for another PC"
    if cmd not in COMMANDS and parse_hotkey(cmd) is None:
        return False, "unknown keys" if cmd.startswith(HOTKEY_PREFIX) else "unknown command"
    # Check the signature before spending the nonce, so a forged packet cannot
    # use up the one the device is about to send.
    try:
        expected = sign(key, pc, nonce, cmd)
    except (TypeError, ValueError):
        return False, "invalid command packet"
    if not hmac.compare_digest(expected, sig):
        return False, "bad signature"
    if not NONCES.consume(nonce):
        return False, "expired or reused"
    return True, ""


# ---------------------------------------------------------------------------
# Running the commands
# ---------------------------------------------------------------------------
_WIN_VK = {
    "media_play_pause": 0xB3, "media_next": 0xB0, "media_prev": 0xB1,
    "media_stop": 0xB2, "volume_up": 0xAF, "volume_down": 0xAE, "volume_mute": 0xAD,
}
_NO_WINDOW = 0x08000000  # CREATE_NO_WINDOW: no console flash from the windowed exe

_WIN_KEY_VK = {
    "ctrl": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B, "space": 0x20, "enter": 0x0D,
    "tab": 0x09, "esc": 0x1B, "backspace": 0x08, "delete": 0x2E, "insert": 0x2D,
    "home": 0x24, "end": 0x23, "pageup": 0x21, "pagedown": 0x22, "up": 0x26, "down": 0x28,
    "left": 0x25, "right": 0x27, "printscreen": 0x2C,
}
# Without KEYEVENTF_EXTENDEDKEY these arrive as their numpad twins.
_WIN_EXTENDED = {"win", "delete", "insert", "home", "end", "pageup", "pagedown",
                 "up", "down", "left", "right"}


def _win_vk(key):
    if key in _WIN_KEY_VK:
        return _WIN_KEY_VK[key]
    if len(key) == 1:
        return ord(key.upper())  # A-Z and 0-9 share their ASCII codes
    return 0x70 + int(key[1:]) - 1  # f1..f24


def _win_hotkey(keys):
    # Goes to the foreground window; Windows drops it for elevated windows
    # unless the companion runs elevated too.
    user32 = ctypes.windll.user32
    for key in keys:
        user32.keybd_event(_win_vk(key), 0, 1 if key in _WIN_EXTENDED else 0, 0)
    for key in reversed(keys):
        user32.keybd_event(_win_vk(key), 0, (1 if key in _WIN_EXTENDED else 0) | 2, 0)


def _win_suspend():
    # SetSuspendState needs SE_SHUTDOWN_NAME enabled in our token.
    import win32api
    import win32security
    token = win32security.OpenProcessToken(
        win32api.GetCurrentProcess(),
        win32security.TOKEN_ADJUST_PRIVILEGES | win32security.TOKEN_QUERY)
    luid = win32security.LookupPrivilegeValue(None, win32security.SE_SHUTDOWN_NAME)
    win32security.AdjustTokenPrivileges(token, False, [(luid, win32security.SE_PRIVILEGE_ENABLED)])
    if not ctypes.windll.powrprof.SetSuspendState(False, False, False):
        raise OSError("SetSuspendState failed")


def _run_windows(cmd):
    keys = parse_hotkey(cmd)
    if keys:
        _win_hotkey(keys)
    elif cmd in _WIN_VK:
        user32 = ctypes.windll.user32
        user32.keybd_event(_WIN_VK[cmd], 0, 0, 0)
        user32.keybd_event(_WIN_VK[cmd], 0, 2, 0)  # KEYEVENTF_KEYUP
    elif cmd == "lock":
        if not ctypes.windll.user32.LockWorkStation():
            raise OSError("LockWorkStation failed")
    elif cmd == "sleep":
        _win_suspend()
    elif cmd in ("restart", "shutdown"):
        flag = "/r" if cmd == "restart" else "/s"
        subprocess.Popen(["shutdown", flag, "/t", "0"], creationflags=_NO_WINDOW)


_LINUX = {
    "media_play_pause": ["playerctl", "play-pause"],
    "media_next": ["playerctl", "next"],
    "media_prev": ["playerctl", "previous"],
    "media_stop": ["playerctl", "stop"],
    "volume_up": ["pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%"],
    "volume_down": ["pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%"],
    "volume_mute": ["pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle"],
    "lock": ["loginctl", "lock-session"],
    "sleep": ["systemctl", "suspend"],
    "restart": ["systemctl", "reboot"],
    "shutdown": ["systemctl", "poweroff"],
}


# xdotool key names (X11 keysyms) where ours differ; X11 only, not Wayland.
_XDOTOOL_KEYS = {
    "win": "super", "enter": "Return", "tab": "Tab", "esc": "Escape", "backspace": "BackSpace",
    "delete": "Delete", "insert": "Insert", "home": "Home", "end": "End", "pageup": "Prior",
    "pagedown": "Next", "up": "Up", "down": "Down", "left": "Left", "right": "Right",
    "printscreen": "Print",
}


def _run_linux(cmd):
    keys = parse_hotkey(cmd)
    if keys:
        names = [_XDOTOOL_KEYS.get(k, k.upper() if k.startswith("f") and k[1:].isdigit() else k)
                 for k in keys]
        argv = ["xdotool", "key", "+".join(names)]
    else:
        argv = _LINUX[cmd]
    subprocess.run(argv, check=True, timeout=10,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_command(cmd):
    """Run one command now. Raises on failure."""
    if os.environ.get("TOUCHDECK_DRY_RUN") == "1":
        print("pc_control: DRY RUN %s" % cmd)
        return
    if sys.platform == "win32":
        _run_windows(cmd)
    else:
        _run_linux(cmd)
    if cmd.startswith(("volume_", "media_")):
        pc_state.STATE.request_poll()


def execute(cmd):
    """Run or schedule a verified command. Returns (ok, message)."""
    if cmd in DEFERRED:
        def later():
            try:
                run_command(cmd)
            except Exception as exc:
                print("pc_control: %s failed: %s" % (cmd, exc))
        threading.Timer(0.5, later).start()
        return True, ""
    try:
        run_command(cmd)
        return True, ""
    except FileNotFoundError as exc:
        return False, "missing tool: %s" % (exc.filename or exc)
    except Exception as exc:
        return False, str(exc)[:60]


# ---------------------------------------------------------------------------
# Listener
# ---------------------------------------------------------------------------
def listen(state, stop):
    """Serve commands until stop is set. Runs on its own thread."""
    port = int(state.get_config().get("cmd_port") or DEFAULT_CMD_PORT)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", port))
    except OSError as exc:
        print("pc_control: cannot listen on UDP %d: %s" % (port, exc))
        return
    sock.settimeout(1.0)
    print("pc_control: listening for device commands on UDP %d" % port)
    while not stop.is_set():
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError:
            continue
        try:
            packet = json.loads(data.decode("utf-8"))
            if not isinstance(packet, dict):
                continue
        except Exception:
            continue
        try:
            ok, why = verify(state.get_config(), packet, addr)
            if ok:
                ok, why = execute(packet["cmd"])
                print("pc_control: %s -> %s %s" % (packet["cmd"], "ok" if ok else "failed", why))
            else:
                print("pc_control: refused %s from %s: %s" % (packet.get("cmd"), addr[0], why))
        except Exception as exc:
            print("pc_control: command packet failed: %s" % exc)
            ok, why = False, "invalid command packet"
        reply = {"ack": packet.get("nonce", ""), "ok": ok, "msg": why}
        try:
            sock.sendto(json.dumps(reply).encode("utf-8"), addr)
        except OSError:
            pass
    sock.close()
