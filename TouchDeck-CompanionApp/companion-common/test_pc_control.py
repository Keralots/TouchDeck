import json
import os
import threading
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pc_control  # noqa: E402

KEY = "11" * 32
CFG = {"pc_id": "0123456789ab", "pc_key": KEY, "esp32_ip": "127.0.0.1"}


def packet(nonce, cmd="media_play_pause", key=KEY, pc="0123456789ab"):
    return {"pc": pc, "cmd": cmd, "nonce": nonce, "sig": pc_control.sign(key, pc, nonce, cmd)}


class PcControlTest(unittest.TestCase):
    def test_missing_sender_is_only_allowed_for_usb(self):
        nonce = pc_control.NONCES.issue()
        self.assertEqual((False, "unknown device"), pc_control.verify(CFG, packet(nonce), None))
        self.assertTrue(pc_control.verify(CFG, packet(nonce), via_usb=True)[0])

    def test_sender_selects_only_its_own_key(self):
        config = dict(CFG, esp32_ip="deck.local:80", panels=[
            {"address": "tablet.local:8080", "key": "22" * 32}])
        addresses = {"deck.local": "192.168.1.10", "tablet.local": "192.168.1.11"}
        with mock.patch.object(pc_control.socket, "gethostbyname", side_effect=addresses.__getitem__):
            for ip, key in (("192.168.1.10", KEY), ("192.168.1.11", "22" * 32)):
                nonce = pc_control.NONCES.issue()
                wrong = "22" * 32 if key == KEY else KEY
                self.assertEqual((False, "bad signature"),
                                 pc_control.verify(config, packet(nonce, key=wrong), (ip, 1234)))
                self.assertEqual((True, ""),
                                 pc_control.verify(config, packet(nonce, key=key), (ip, 1234)))
            nonce = pc_control.NONCES.issue()
            self.assertEqual((False, "unknown device"),
                             pc_control.verify(config, packet(nonce), ("192.168.1.99", 1234)))
            self.assertTrue(pc_control.verify(config, packet(nonce), ("192.168.1.10", 1234))[0])

    def test_sender_resolution_failure_and_unpaired_panel(self):
        config = dict(CFG, esp32_ip="missing.local", panels=[
            {"address": "tablet.local:8080", "key": ""},
            {"address": "tablet.local:8081", "key": KEY}])
        nonce = pc_control.NONCES.issue()
        with mock.patch.object(pc_control.socket, "gethostbyname",
                               side_effect=[OSError("offline"), "192.168.1.11"]) as resolve:
            self.assertEqual((False, "unknown device"),
                             pc_control.verify(config, packet(nonce), ("192.168.1.99", 1234)))
            self.assertEqual(2, resolve.call_count)
        with mock.patch.object(pc_control.socket, "gethostbyname",
                               side_effect=[OSError("offline"), "192.168.1.11"]):
            self.assertEqual((False, "this PC is not paired"),
                             pc_control.verify(config, packet(nonce), ("192.168.1.11", 1234)))
        self.assertTrue(pc_control.NONCES.consume(nonce))

    def test_signed_command_with_fresh_nonce_passes_once(self):
        n = pc_control.NONCES.issue()
        self.assertEqual((True, ""), pc_control.verify(CFG, packet(n), ("127.0.0.1", 4210)))
        self.assertEqual((False, "expired or reused"), pc_control.verify(CFG, packet(n), ("127.0.0.1", 4210)))

    def test_unknown_nonce_is_refused(self):
        self.assertFalse(pc_control.verify(CFG, packet("deadbeefdeadbeef"), ("127.0.0.1", 4210))[0])

    def test_bad_signature_keeps_the_nonce(self):
        n = pc_control.NONCES.issue()
        self.assertEqual("bad signature", pc_control.verify(CFG, packet(n, key="22" * 32), ("127.0.0.1", 4210))[1])
        self.assertTrue(pc_control.verify(CFG, packet(n), ("127.0.0.1", 4210))[0])

    def test_signature_covers_the_command(self):
        n = pc_control.NONCES.issue()
        p = packet(n, cmd="media_next")
        p["cmd"] = "shutdown"
        self.assertEqual("bad signature", pc_control.verify(CFG, p, ("127.0.0.1", 4210))[1])

    def test_other_pc_unpaired_and_unknown_commands(self):
        n = pc_control.NONCES.issue()
        self.assertFalse(pc_control.verify(CFG, packet(n, pc="ffffffffffff"), ("127.0.0.1", 4210))[0])
        self.assertFalse(pc_control.verify({"pc_id": "0123456789ab"}, packet(n), ("127.0.0.1", 4210))[0])
        self.assertFalse(pc_control.verify(CFG, packet(n, cmd="format_disk"), ("127.0.0.1", 4210))[0])

    def test_hotkey_parsing(self):
        self.assertEqual(["ctrl", "shift", "m"], pc_control.parse_hotkey("hotkey:ctrl+shift+m"))
        self.assertEqual(["win", "f24"], pc_control.parse_hotkey("hotkey:win+f24"))
        for bad in ("hotkey:", "hotkey:ctrl+", "hotkey:ctrl+ctrl", "hotkey:f25", "hotkey:CTRL+m",
                    "hotkey:a+b+c+d+e+f", "hotkey:ctrl shift", "ctrl+m", None):
            with self.subTest(bad=bad):
                self.assertIsNone(pc_control.parse_hotkey(bad))

    def test_signed_hotkey_passes_and_bad_keys_are_refused(self):
        n = pc_control.NONCES.issue()
        self.assertEqual((True, ""), pc_control.verify(CFG, packet(n, cmd="hotkey:ctrl+alt+t"), ("127.0.0.1", 4210)))
        n = pc_control.NONCES.issue()
        self.assertEqual((False, "unknown keys"), pc_control.verify(CFG, packet(n, cmd="hotkey:ctrl+nope"), ("127.0.0.1", 4210)))

    def test_hotkey_presses_in_order_and_releases_in_reverse(self):
        user32 = mock.Mock()
        with mock.patch.object(pc_control, "ctypes", create=True) as ct:
            ct.windll.user32 = user32
            pc_control._win_hotkey(["ctrl", "up", "a"])
        self.assertEqual([mock.call(0x11, 0, 0, 0), mock.call(0x26, 0, 1, 0), mock.call(0x41, 0, 0, 0),
                          mock.call(0x41, 0, 2, 0), mock.call(0x26, 0, 3, 0), mock.call(0x11, 0, 2, 0)],
                         user32.keybd_event.call_args_list)

    def test_hotkey_on_linux_uses_xdotool_names(self):
        with mock.patch.object(pc_control.subprocess, "run") as run:
            pc_control._run_linux("hotkey:ctrl+pageup+f5")
        self.assertEqual(["xdotool", "key", "ctrl+Prior+F5"], run.call_args[0][0])

    def test_firmware_message_layout(self):
        # Same input as the device's mbedtls HMAC: "pc_id|nonce|cmd".
        import hashlib
        import hmac
        expect = hmac.new(bytes.fromhex(KEY), b"0123456789ab|abc|lock", hashlib.sha256).hexdigest()
        self.assertEqual(expect, pc_control.sign(KEY, "0123456789ab", "abc", "lock"))


    def test_hostile_fields_are_refused_without_spending_nonce(self):
        cases = [("sig", "\u00e9" * 64), ("sig", 123), ("sig", ["a"] * 64),
                 ("sig", "g" * 64), ("sig", "a" * 63), ("sig", "a" * 65),
                 ("nonce", ["nonce"]), ("nonce", "\ud800"),
                 ("cmd", ["media_next"]), ("pc", [CFG["pc_id"]])]
        for field, value in cases:
            with self.subTest(field=field, value=value):
                nonce = pc_control.NONCES.issue()
                valid = packet(nonce)
                self.assertFalse(pc_control.verify(CFG, dict(valid, **{field: value}), ("127.0.0.1", 4210))[0])
                self.assertTrue(pc_control.verify(CFG, valid, ("127.0.0.1", 4210))[0])
        for value in (None, [], "command", 1):
            with self.subTest(packet=value):
                self.assertFalse(pc_control.verify(CFG, value, ("127.0.0.1", 4210))[0])

    def test_listener_survives_verification_and_execution_exceptions(self):
        for failing in ("verify", "execute"):
            with self.subTest(failing=failing):
                stop = threading.Event()
                sock = mock.Mock()
                first = packet(pc_control.NONCES.issue())
                second = packet(pc_control.NONCES.issue())
                sock.recvfrom.side_effect = [
                    (json.dumps(p).encode("utf-8"), ("127.0.0.1", 4210))
                    for p in (first, second)]
                replies = []

                def send(data, addr):
                    replies.append(json.loads(data))
                    if len(replies) == 2:
                        stop.set()

                sock.sendto.side_effect = send
                state = mock.Mock()
                state.get_config.return_value = CFG
                effects = [RuntimeError("packet failed"), (True, "")]
                with mock.patch.object(pc_control.socket, "socket", return_value=sock), \
                        mock.patch.object(pc_control, "verify", return_value=(True, "")) as verify, \
                        mock.patch.object(pc_control, "execute", return_value=(True, "")) as execute, \
                        mock.patch("builtins.print") as log:
                    (verify if failing == "verify" else execute).side_effect = effects
                    pc_control.listen(state, stop)
                self.assertEqual([False, True], [reply["ok"] for reply in replies])
                self.assertEqual(("127.0.0.1", 4210), verify.call_args.args[2])
                self.assertTrue(any("packet failed" in str(call) for call in log.call_args_list))
                sock.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
