import hashlib
import io
import pathlib
import tempfile
import unittest

from PIL import Image

import walls


class FakeDeck:
    def __init__(self, source=2, mounted=True):
        self.source, self.mounted = source, mounted
        self.files = {("m", "l"): {}, ("m", "p"): {}, ("d", "l"): {}, ("d", "p"): {}}

    def walls(self, orient, gallery, offset=0, limit=1):
        return {"source": self.source, "count": len(self.files[gallery, orient]), "sd": {"mounted": self.mounted}}

    def gallery(self, orient, gallery):
        return [(name, len(data)) for name, data in self.files[gallery, orient].items()]

    def file(self, orient, gallery, name):
        return self.files[gallery, orient][name]

    def upload(self, orient, data, name=None, index=0, count=0):
        gallery = self.files["d" if name else "m", orient]
        if name in gallery:
            return "already there"
        gallery[name or "%08x.jpg" % (len(gallery) + 1)] = data
        return "stored"


class Job:
    cancelled = False

    def __init__(self):
        self.counts = {"added": 0, "skipped": 0, "failed": 0}

    def set(self, **fields):
        pass

    def count(self, key, line=None):
        self.counts[key] += 1


class FolderTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.dir.name)
        (root / "sub").mkdir()
        Image.new("RGB", (1200, 800), (20, 90, 160)).save(root / "land.jpg")
        Image.new("RGB", (600, 900), (160, 60, 30)).save(root / "sub" / "port.png")
        exif = Image.Exif()
        exif[0x0112] = 6  # stored landscape, shown as portrait
        Image.new("RGB", (900, 600), (40, 140, 60)).save(root / "rotated.jpg", exif=exif)
        (root / "broken.jpg").write_bytes(b"\xff\xd8 not a jpeg")
        (root / "notes.txt").write_text("text")
        self.root = root

    def tearDown(self):
        self.dir.cleanup()

    def snapshot(self):
        return {p: (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                for p in self.root.rglob("*") if p.is_file()}

    def test_match_shape_keeps_originals_and_dedupes(self):
        deck, before = FakeDeck(), self.snapshot()
        job = Job()
        walls.run_folder(job, deck, self.dir.name, "match", True)
        self.assertEqual(job.counts, {"added": 3, "skipped": 0, "failed": 1})
        self.assertEqual(len(deck.files["m", "l"]), 1)
        self.assertEqual(len(deck.files["m", "p"]), 2)
        for data in deck.files["m", "p"].values():
            with Image.open(io.BytesIO(data)) as img:
                self.assertEqual((img.size, img.format), ((320, 480), "JPEG"))
        job = Job()
        walls.run_folder(job, deck, self.dir.name, "match", True)
        self.assertEqual(job.counts, {"added": 0, "skipped": 3, "failed": 1})
        self.assertEqual(self.snapshot(), before)

    def test_without_subfolders(self):
        deck = FakeDeck()
        walls.run_folder(Job(), deck, self.dir.name, "l", False)
        self.assertEqual(len(deck.files["m", "l"]), 2)

    def test_refused_while_deck_shows_daily_only(self):
        with self.assertRaises(walls.DeckError):
            walls.run_folder(Job(), FakeDeck(source=1), self.dir.name, "match", True)

    def test_download_needs_a_card(self):
        with self.assertRaises(walls.DeckError):
            walls.run_download(Job(), FakeDeck(mounted=False), ["l"], 5)


if __name__ == "__main__":
    unittest.main()
