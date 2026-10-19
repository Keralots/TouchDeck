import http.client
from base64 import b64decode
import unittest
import zlib

import art_server
import udp_stream
from unittest import mock


class ArtServerTest(unittest.TestCase):
    def setUp(self):
        self.art = art_server.ArtServer()
        self.art.start("127.0.0.1", 0)
        # A 96x32 PNG: red square in the middle, blue on either side.
        self.source = b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAGAAAAAgCAIAAABiouoDAAAASklEQVR4nO3QsQ0AAAzCsPz/dHsFmyVmQK5umm17"
            "rf/PBwABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABAgQIECBAgAABapYHDzD0poQHXYwAAAAASUVORK5CYII=")
        self.id = self.art.set_current(self.source)

    def tearDown(self):
        self.art.stop()

    def get(self, path):
        connection = http.client.HTTPConnection("127.0.0.1", self.art.port, timeout=2)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_crc_crop_rgb565_and_size_clamps(self):
        self.assertEqual(self.id, "%08x" % zlib.crc32(self.source))
        for requested, size in ((1, 32), (80, 80), (999, 200)):
            status, headers, body = self.get("/art?id=%s&s=%d" % (self.id, requested))
            self.assertEqual(status, 200)
            self.assertEqual(headers["Content-Type"], "application/octet-stream")
            self.assertEqual(int(headers["Content-Length"]), size * size * 2)
            self.assertEqual(body, b"\x00\xf8" * size * size)

    def test_only_current_id_and_art_route(self):
        for path in ("/", "/art?id=bad&s=80", "/art?id=00000000&s=80"):
            self.assertEqual(self.get(path)[0], 404)
        self.assertEqual(self.get("/art?id=%s&s=invalid" % self.id)[0], 400)
        self.art.set_current(None)
        self.assertEqual(self.get("/art?id=%s&s=80" % self.id)[0], 404)
        self.art.set_current(b"different cover")
        self.assertEqual(self.get("/art?id=%s&s=80" % self.id)[0], 404)

    def test_cache_is_bounded_and_invalid_source_is_not_served(self):
        first = self.art.pixels(self.id, 32)
        self.assertIs(first, self.art.pixels(self.id, 32))
        for size in range(33, 42):
            self.art.pixels(self.id, size)
        self.assertEqual(len(self.art._cache), 6)
        invalid = self.art.set_current(b"not an image")
        self.assertEqual(self.get("/art?id=%s&s=80" % invalid)[0], 404)

    def test_busy_port_falls_back_and_identity_follows_lifecycle(self):
        other = art_server.ArtServer()
        try:
            other.start("127.0.0.1", self.art.port)
            self.assertNotEqual(other.port, self.art.port)
            with mock.patch.object(udp_stream, "ART", other):
                self.assertEqual(udp_stream.packet_identity({})["art_port"], other.port)
                other.stop()
                self.assertNotIn("art_port", udp_stream.packet_identity({}))
        finally:
            other.stop()


if __name__ == "__main__":
    unittest.main()
