"""Wallpapers prepared on the PC and uploaded to the deck.

The deck stores baseline 480x320 / 320x480 JPEGs. Downloaded pictures go to
its daily archive under the names its own downloads use, so it dedupes them;
a folder's pictures go to My pictures. Source files are only ever read.
"""

import io
import json
import pathlib
import threading
import uuid
import zlib
from urllib import error as urllib_error
from urllib import request as urllib_request
from urllib.parse import urlencode

SIZES = {"l": (480, 320), "p": (320, 480)}
EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif", ".tif", ".tiff"}
WEB_UA = "TouchDeck-Companion (https://github.com/Keralots/TouchDeck)"
SPOTLIGHT = (
    "https://fd.api.iris.microsoft.com/v4/api/selection"
    "?placement=88000820&bcnt=4&country=US&locale=en-US&fmt=json"
)
LOG_LINES = 40

_DIRECT = urllib_request.build_opener(urllib_request.ProxyHandler({}))


class DeckError(Exception):
    pass


def encode(img, size):
    from PIL import Image, ImageOps
    img = ImageOps.fit(img.convert("RGB"), size, Image.Resampling.LANCZOS)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85, optimize=True, progressive=False)
    return out.getvalue()


def fetch(url):
    req = urllib_request.Request(url, headers={"User-Agent": WEB_UA})
    with urllib_request.urlopen(req, timeout=60) as response:
        return response.read()


class Deck:
    def __init__(self, address, ua):
        self.base = "http://%s" % address
        self.ua = ua

    def _open(self, path, data=None, headers=None, timeout=10):
        req = urllib_request.Request(self.base + path, data=data, headers=dict(headers or {}, **{"User-Agent": self.ua}))
        try:
            with _DIRECT.open(req, timeout=timeout) as response:
                return response.read()
        except urllib_error.HTTPError as exc:
            try:
                message = json.loads(exc.read() or b"{}").get("message")
            except ValueError:
                message = None
            raise DeckError(message or "the deck answered HTTP %d" % exc.code) from None
        except (urllib_error.URLError, OSError) as exc:
            raise DeckError("the deck does not answer (%s)" % getattr(exc, "reason", exc)) from None

    def walls(self, orient, gallery, offset=0, limit=1):
        query = urlencode({"o": orient, "gallery": gallery, "offset": offset, "limit": limit})
        return json.loads(self._open("/api/deck/walls?" + query))

    def gallery(self, orient, gallery):
        """All (name, size) pairs of one gallery; names without the m/ d/ prefix."""
        out, offset = [], 0
        while True:
            page = self.walls(orient, gallery, offset, 96)
            out += [(f["n"].split("/")[-1], f["s"]) for f in page.get("files", [])]
            offset += 96
            if offset >= page.get("count", 0):
                return out

    def file(self, orient, gallery, name):
        return self._open("/api/deck/walls/file?" + urlencode({"o": orient, "gallery": gallery, "n": name}))

    def upload(self, orient, data, name=None, index=0, count=0):
        boundary = uuid.uuid4().hex
        body = (
            ("--%s\r\nContent-Disposition: form-data; name=\"wall\"; filename=\"%s\"\r\n"
             "Content-Type: image/jpeg\r\n\r\n" % (boundary, name or "wall.jpg")).encode()
            + data + ("\r\n--%s--\r\n" % boundary).encode())
        query = {"o": orient}
        query.update({"gallery": "d", "n": name} if name else {"i": index, "c": count})
        reply = self._open("/api/deck/walls?" + urlencode(query), body,
                           {"Content-Type": "multipart/form-data; boundary=" + boundary}, 60)
        return json.loads(reply or b"{}").get("message", "")


def deck_summary(deck):
    first = deck.walls("l", "m")
    counts = {"ml": first.get("count", 0)}
    for key in ("mp", "dl", "dp"):
        counts[key] = deck.walls(key[1], key[0]).get("count", 0)
    sd = first.get("sd") or {}
    return {"success": True, "source": first.get("source", 0), "sd_enabled": bool(sd.get("enabled")),
            "sd_mounted": bool(sd.get("mounted")), "free": first.get("free"), "total": first.get("total"),
            "counts": counts}


class WallJob:
    """One picture job at a time, run in a thread and polled by the UI."""

    def __init__(self):
        self._lock = threading.Lock()
        self._cancel = threading.Event()
        self._state = {"running": False, "log": []}

    def status(self):
        with self._lock:
            return dict(self._state, log=list(self._state["log"]))

    def start(self, kind, fn, *args):
        with self._lock:
            if self._state["running"]:
                raise ValueError("A picture job is already running.")
            self._state = {"running": True, "kind": kind, "done": 0, "total": 0, "added": 0, "skipped": 0,
                           "failed": 0, "error": False, "message": "Starting...", "log": []}
        self._cancel.clear()
        threading.Thread(target=self._run, args=(fn, args), daemon=True, name="walls").start()

    def cancel(self):
        self._cancel.set()

    @property
    def cancelled(self):
        return self._cancel.is_set()

    def _run(self, fn, args):
        error = False
        try:
            message = fn(self, *args)
        except Exception as exc:
            message, error = str(exc), True
        with self._lock:
            self._state.update(running=False, error=error, message=message)

    def set(self, **fields):
        with self._lock:
            self._state.update(fields)

    def count(self, key, line=None):
        with self._lock:
            self._state[key] += 1
            if line:
                self._state["log"] = (self._state["log"] + [line])[-LOG_LINES:]


def spotlight(job, orients, count):
    seen = set()
    for _ in range(count * 3):
        try:
            items = json.loads(fetch(SPOTLIGHT))["batchrsp"]["items"]
        except Exception as exc:
            job.count("failed", "Spotlight: %s" % exc)
            continue
        for item in items:
            ad = json.loads(item["item"]).get("ad", {})
            for orient, key in (("l", "landscapeImage"), ("p", "portraitImage")):
                url = ad.get(key, {}).get("asset", "")
                if orient in orients and url.startswith("https://") and url not in seen:
                    seen.add(url)
                    # Named by the URL's crc32 like the deck's own downloads.
                    yield orient, "%08x.jpg" % zlib.crc32(url.encode()), url


def run_download(job, deck, orients, count):
    from PIL import Image
    if not deck.walls("l", "d").get("sd", {}).get("mounted"):
        raise DeckError("Downloaded pictures go to the deck's microSD card, and it has none mounted.")
    have = {o: {name for name, _ in deck.gallery(o, "d")} for o in orients}
    got = dict.fromkeys(orients, 0)
    job.set(total=count * len(orients), message="Downloading...")
    for orient, name, url in spotlight(job, orients, count):
        if job.cancelled:
            return "Cancelled, %d new pictures on the deck." % sum(got.values())
        if all(got[o] >= count for o in orients):
            break
        if got[orient] >= count:
            continue
        if name in have[orient]:
            job.count("skipped")
            continue
        try:
            with Image.open(io.BytesIO(fetch(url))) as img:
                data = encode(img, SIZES[orient])
        except Exception as exc:
            job.count("failed", "%s: %s" % (name, exc))
            continue
        try:
            stored = deck.upload(orient, data, name) != "already there"
        except DeckError as exc:
            raise DeckError("The deck refused a picture: %s" % exc) from None
        have[orient].add(name)
        if stored:
            got[orient] += 1
            job.count("added")
        else:
            job.count("skipped")
        job.set(done=sum(got.values()))
    short = sum(count - n for n in got.values())
    return "%d new pictures on the deck%s." % (sum(got.values()), ", %d short: Spotlight kept repeating pictures, try again later" % short if short else "")


class _OnDeck:
    """Own-gallery pictures, compared by size first and bytes only on a size match."""

    def __init__(self, deck, orient):
        self.deck, self.orient = deck, orient
        self.sizes = {}
        for name, size in deck.gallery(orient, "m"):
            self.sizes.setdefault(size, []).append(name)
        self.sent = set()

    def has(self, data):
        if zlib.crc32(data) in self.sent:
            return True
        for name in self.sizes.get(len(data), []):
            try:
                if self.deck.file(self.orient, "m", name) == data:
                    return True
            except DeckError:
                pass
        return False


def run_folder(job, deck, folder, mode, recursive):
    from PIL import Image, ImageOps
    root = pathlib.Path(folder).expanduser()
    if not root.is_dir():
        raise ValueError("Folder not found: %s" % folder)
    if deck.walls("l", "m").get("source") == 1:
        raise DeckError("The deck shows downloaded pictures only. Set Pictures to My pictures or Both in its portal first.")
    files = sorted(p for p in (root.rglob("*") if recursive else root.glob("*"))
                   if p.suffix.lower() in EXTS and p.is_file())
    if not files:
        raise ValueError("No pictures in %s." % root)
    orients = ["l", "p"] if mode in ("both", "match") else [mode]
    job.set(total=len(files), message="Checking the deck...")
    on_deck = {o: _OnDeck(deck, o) for o in orients}
    job.set(message="Converting and sending...")
    for index, path in enumerate(files, 1):
        if job.cancelled:
            return "Cancelled after %d of %d files." % (index - 1, len(files))
        try:
            # Opened read-only; the converted copies live in memory.
            with Image.open(path) as img:
                img.draft("RGB", (960, 960))
                img = ImageOps.exif_transpose(img)
            targets = ["l" if img.width >= img.height else "p"] if mode == "match" else orients
            pictures = [(o, encode(img, SIZES[o])) for o in targets]
        except Exception as exc:
            job.count("failed", "%s: %s" % (path.name, exc))
            job.set(done=index)
            continue
        for orient, data in pictures:
            if on_deck[orient].has(data):
                job.count("skipped")
                continue
            try:
                deck.upload(orient, data, index=index, count=len(files))
            except DeckError as exc:
                raise DeckError("The deck refused %s: %s" % (path.name, exc)) from None
            on_deck[orient].sent.add(zlib.crc32(data))
            job.count("added")
        job.set(done=index)
    return "%d files done." % len(files)
