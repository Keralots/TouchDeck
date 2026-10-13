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

DEFAULT_CMD_PORT = 4211
NONCE_TTL = 30.0
COMMANDS = ("media_play_pause", "media_next", "media_prev", "media_stop",
            "volume_up", "volume_down", "volume_mute", "lock", "sleep",
            "restart", "shutdown")
# These would cut the acknowledgement off, so they start a moment after it.
DEFERRED = ("lock", "sleep", "restart", "shutdown")


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


def verify(config, packet):
    """Return (ok, reason) for a command packet."""
    if not isinstance(config, dict) or not isinstance(packet, dict):
        return False, "invalid command packet"
    pc, cmd, nonce, sig = (packet.get(field) for field in ("pc", "cmd", "nonce", "sig"))
    if not all(isinstance(value, str) for value in (pc, cmd, nonce)):
        return False, "invalid command packet"
    if (not isinstance(sig, str) or len(sig) != 64
            or any(ch not in "0123456789abcdefABCDEF" for ch in sig)):
        return False, "bad signature"
    key = config.get("pc_key") or ""
    if not key:
        return False, "this PC is not paired"
    if pc != config.get("pc_id"):
        return False, "command for another PC"
    if cmd not in COMMANDS:
        return False, "unknown command"
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
    if cmd in _WIN_VK:
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


def _run_linux(cmd):
    subprocess.run(_LINUX[cmd], check=True, timeout=10,
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
            ok, why = verify(state.get_config(), packet)
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
