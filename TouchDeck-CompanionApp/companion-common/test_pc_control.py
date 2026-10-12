import os
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
