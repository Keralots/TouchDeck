"""Serve the current cover as native RGB565 pixels for the deck."""

from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import re
import threading
from urllib.parse import parse_qs, urlsplit
import zlib

DEFAULT_ART_PORT = 8751


class _ArtServer(ThreadingHTTPServer):
    # Windows SO_REUSEADDR can steal another process's connections instead
    # of failing bind, which would prevent the ephemeral-port fallback.
    allow_reuse_address = False
    daemon_threads = True


class _Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlsplit(self.path)
        query = parse_qs(url.query)
        art_id = query.get("id", [""])[0]
        try:
            size = max(32, min(200, int(query.get("s", ["80"])[0])))
        except ValueError:
            self.send_error(400)
            return
        if url.path != "/art" or not re.fullmatch(r"[0-9a-fA-F]{8}", art_id):
            self.send_error(404)
            return
        body = self.server.art.pixels(art_id.lower(), size)
        if body is None:
            self.send_error(404)
            return
        # No authentication: cover images contain no sensitive companion data.
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class ArtServer:
    def __init__(self):
        self._lock = threading.Lock()
        self._id = None
        self._source = None
        self._cache = OrderedDict()
        self._server = None
        self._thread = None

    @property
    def port(self):
        return self._server.server_address[1] if self._server else None

    def set_current(self, source):
        art_id = "%08x" % zlib.crc32(source) if source else None
        with self._lock:
            self._id, self._source = art_id, source
        return art_id

    def pixels(self, art_id, size):
        size = max(32, min(200, size))
        key = (art_id, size)
        with self._lock:
            if art_id != self._id or not self._source:
                return None
            if key in self._cache:
                self._cache.move_to_end(key)
                return self._cache[key]
            source = self._source
        try:
            from PIL import Image
            with Image.open(BytesIO(source)) as original:
                cover = original.convert("RGB")
                side = min(cover.size)
                left, top = (cover.width - side) // 2, (cover.height - side) // 2
                cover = cover.crop((left, top, left + side, top + side))
                cover = cover.resize((size, size), Image.Resampling.LANCZOS)
            rgb = cover.tobytes()
            data = bytearray(size * size * 2)
            for i in range(size * size):
                red, green, blue = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
                pixel = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
                data[2 * i], data[2 * i + 1] = pixel & 255, pixel >> 8
            body = bytes(data)
        except (ImportError, OSError, ValueError):
            return None
        with self._lock:
            if self._id != art_id:
                return None
            self._cache[key] = body
            while len(self._cache) > 6:
                self._cache.popitem(last=False)
        return body

    def start(self, host="0.0.0.0", port=DEFAULT_ART_PORT):
        if self._server is not None:
            return
        try:
            server = _ArtServer((host, port), _Handler)
        except OSError:
            server = _ArtServer((host, 0), _Handler)
        server.art = self
        self._server = server
        self._thread = threading.Thread(target=server.serve_forever, daemon=True,
                                        name="cover-art")
        self._thread.start()

    def stop(self):
        server, self._server = self._server, None
        if server is not None:
            server.shutdown()
            server.server_close()
            self._thread.join(timeout=1)
        self.set_current(None)


ART = ArtServer()
