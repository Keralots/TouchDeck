import os
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import host_cache  # noqa: E402


class HostCacheTest(unittest.TestCase):
    def setUp(self):
        host_cache.clear()
        self.clock = [100.0]
        patcher = mock.patch.object(host_cache.time, "monotonic", side_effect=lambda: self.clock[0])
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_ip_literal_never_looks_up(self):
        with mock.patch.object(host_cache.socket, "gethostbyname") as resolve:
            self.assertEqual("192.168.0.5", host_cache.resolve("192.168.0.5"))
            self.assertIsNone(host_cache.resolve(""))
        resolve.assert_not_called()

    def test_answer_is_cached_and_refreshed_in_background(self):
        done = threading.Event()

        def lookup(host):
            if resolve.call_count == 2:
                done.set()
                return "10.0.0.2"
            return "10.0.0.1"

        with mock.patch.object(host_cache.socket, "gethostbyname", side_effect=lookup) as resolve:
            self.assertEqual("10.0.0.1", host_cache.resolve("deck.local"))
            self.assertEqual("10.0.0.1", host_cache.resolve("deck.local"))
            self.assertEqual(1, resolve.call_count)
            self.clock[0] += host_cache.TTL
            self.assertEqual("10.0.0.1", host_cache.resolve("deck.local"))  # stale, not blocking
            self.assertTrue(done.wait(2))
            for thread in threading.enumerate():
                if thread.name == "resolve-deck.local":
                    thread.join(2)
            self.assertEqual("10.0.0.2", host_cache.resolve("deck.local"))
            self.assertEqual(2, resolve.call_count)

    def test_failure_is_cached_for_the_retry_window(self):
        with mock.patch.object(host_cache.socket, "gethostbyname", side_effect=OSError("offline")) as resolve:
            self.assertIsNone(host_cache.resolve("gone.local"))
            self.clock[0] += host_cache.RETRY - 1
            self.assertIsNone(host_cache.resolve("gone.local"))
            self.assertIsNone(host_cache.resolve("gone.local", fresh=True))  # forced lookup
            self.assertEqual(2, resolve.call_count)

    def test_forced_lookup_is_rate_limited(self):
        with mock.patch.object(host_cache.socket, "gethostbyname", return_value="10.0.0.1") as resolve:
            host_cache.resolve("deck.local")
            host_cache.resolve("deck.local", fresh=True)
            self.assertEqual(1, resolve.call_count)
            self.clock[0] += host_cache.MIN_REFRESH
            host_cache.resolve("deck.local", fresh=True)
            self.assertEqual(2, resolve.call_count)


if __name__ == "__main__":
    unittest.main()
