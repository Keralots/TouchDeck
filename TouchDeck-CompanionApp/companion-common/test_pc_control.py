import json
import os
import threading
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pc_control  # noqa: E402

KEY = "11" * 32
CFG = {"pc_id": "0123456789ab", "pc_key": KEY}


def packet(nonce, cmd="media_play_pause", key=KEY, pc="0123456789ab"):
    return {"pc": pc, "cmd": cmd, "nonce": nonce, "sig": pc_control.sign(key, pc, nonce, cmd)}


class PcControlTest(unittest.TestCase):
    def test_signed_command_with_fresh_nonce_passes_once(self):
        n = pc_control.NONCES.issue()
        self.assertEqual((True, ""), pc_control.verify(CFG, packet(n)))
        self.assertEqual((False, "expired or reused"), pc_control.verify(CFG, packet(n)))

    def test_unknown_nonce_is_refused(self):
        self.assertFalse(pc_control.verify(CFG, packet("deadbeefdeadbeef"))[0])

    def test_bad_signature_keeps_the_nonce(self):
        n = pc_control.NONCES.issue()
        self.assertEqual("bad signature", pc_control.verify(CFG, packet(n, key="22" * 32))[1])
        self.assertTrue(pc_control.verify(CFG, packet(n))[0])

    def test_signature_covers_the_command(self):
        n = pc_control.NONCES.issue()
        p = packet(n, cmd="media_next")
        p["cmd"] = "shutdown"
        self.assertEqual("bad signature", pc_control.verify(CFG, p)[1])

    def test_other_pc_unpaired_and_unknown_commands(self):
        n = pc_control.NONCES.issue()
        self.assertFalse(pc_control.verify(CFG, packet(n, pc="ffffffffffff"))[0])
        self.assertFalse(pc_control.verify({"pc_id": "0123456789ab"}, packet(n))[0])
        self.assertFalse(pc_control.verify(CFG, packet(n, cmd="format_disk"))[0])

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
                self.assertFalse(pc_control.verify(CFG, dict(valid, **{field: value}))[0])
                self.assertTrue(pc_control.verify(CFG, valid)[0])
        for value in (None, [], "command", 1):
            with self.subTest(packet=value):
                self.assertFalse(pc_control.verify(CFG, value)[0])

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
                self.assertTrue(any("packet failed" in str(call) for call in log.call_args_list))
                sock.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
