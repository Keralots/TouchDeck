#!/usr/bin/env python3
"""Prepare wallpapers for TouchDeck on a PC.

Pictures are centre-cropped to 480x320 (landscape) and/or 320x480 (portrait)
and saved as baseline JPEG with 8-hex names, the format the deck expects.

  prepare SRC OUT     your photos -> OUT/l, OUT/p
  spotlight N OUT     N Windows Spotlight pictures per orientation -> OUT/dl, OUT/dp
  upload DIR HOST     send DIR/l and DIR/p to the deck's own gallery over WiFi
                      (--daily: DIR/dl, DIR/dp into the daily archive)

Copy OUT's folders to the microSD card under /touchdeck/wall/ (l, p = own
pictures, dl, dp = daily archive), or use "upload". Needs Pillow.
"""
import argparse
import io
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request
import uuid
import zlib

from PIL import Image, ImageOps

SIZES = {"l": (480, 320), "p": (320, 480)}
EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff", ".heic"}
SPOTLIGHT = (
    "https://fd.api.iris.microsoft.com/v4/api/selection"
    "?placement=88000820&bcnt=4&country=US&locale=en-US&fmt=json"
)
UA = "TouchDeck-wall-tool"


def encode(img, size):
    img = ImageOps.fit(img.convert("RGB"), size, Image.LANCZOS)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85, optimize=True, progressive=False)
    return out.getvalue()


def save(data, folder, name=None):
    folder.mkdir(parents=True, exist_ok=True)
    name = name or "%08x.jpg" % zlib.crc32(data)
    path = folder / name
    if path.exists():
        return False
    path.write_bytes(data)
    return True


def cmd_prepare(args):
    orients = ["l", "p"] if args.orient == "both" else [args.orient]
    files = sorted(p for p in pathlib.Path(args.src).rglob("*") if p.suffix.lower() in EXTS)
    added = 0
    for f in files:
        try:
            img = ImageOps.exif_transpose(Image.open(f))
        except Exception as e:
            print("skip %s: %s" % (f.name, e))
            continue
        for o in orients:
            added += save(encode(img, SIZES[o]), pathlib.Path(args.out) / o)
    print("%d pictures from %d files" % (added, len(files)))


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def cmd_spotlight(args):
    out = pathlib.Path(args.out)
    orients = ["l", "p"] if args.orient == "both" else [args.orient]
    got = {o: 0 for o in orients}
    seen = set()
    for _ in range(args.count * 3):
        if all(got[o] >= args.count for o in orients):
            break
        reply = json.loads(fetch(SPOTLIGHT))
        for item in reply["batchrsp"]["items"]:
            ad = json.loads(item["item"]).get("ad", {})
            for o, key in (("l", "landscapeImage"), ("p", "portraitImage")):
                url = ad.get(key, {}).get("asset", "")
                if o not in got or got[o] >= args.count or not url.startswith("https://") or url in seen:
                    continue
                seen.add(url)
                # Named by the URL's crc32 like the deck's own downloads.
                name = "%08x.jpg" % zlib.crc32(url.encode())
                if save(encode(Image.open(io.BytesIO(fetch(url))), SIZES[o]), out / ("d" + o), name):
                    got[o] += 1
                    print("%s %s (%d/%d)" % (o, name, got[o], args.count))
    print(", ".join("%s: %d new" % kv for kv in got.items()))


def post_jpeg(host, orient, name, data, daily=False):
    boundary = uuid.uuid4().hex
    body = (
        ("--%s\r\nContent-Disposition: form-data; name=\"wall\"; filename=\"%s\"\r\n"
         "Content-Type: image/jpeg\r\n\r\n" % (boundary, name)).encode()
        + data
        + ("\r\n--%s--\r\n" % boundary).encode()
    )
    req = urllib.request.Request(
        "http://%s/api/deck/walls?o=%s" % (host, orient) + ("&gallery=d&n=" + name if daily else ""),
        data=body,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary, "User-Agent": UA},
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read() or b"{}")


def cmd_upload(args):
    ok = fail = 0
    for o in ("l", "p"):
        folder = pathlib.Path(args.dir) / (("d" if args.daily else "") + o)
        for f in sorted(folder.glob("*.jpg")) if folder.is_dir() else []:
            t0 = time.monotonic()
            try:
                reply = post_jpeg(args.host, o, f.name, f.read_bytes(), args.daily)
                good = reply.get("success", True)
            except urllib.error.HTTPError as e:
                reply, good = {"error": e.read().decode(errors="replace") or str(e)}, False
            except Exception as e:
                reply, good = {"error": str(e)}, False
            ok += bool(good)
            fail += not good
            note = reply.get("message", "") if good and args.daily else ""
            print("%s %s %s %d ms" % (o, f.name, (note or "ok") if good else reply, (time.monotonic() - t0) * 1000))
            if not good and args.stop:
                sys.exit(1)
    print("%d uploaded, %d failed" % (ok, fail))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare", help="convert your photos")
    p.add_argument("src")
    p.add_argument("out")
    p.add_argument("--orient", choices=["l", "p", "both"], default="both")
    p.set_defaults(fn=cmd_prepare)
    p = sub.add_parser("spotlight", help="download Windows Spotlight pictures")
    p.add_argument("count", type=int)
    p.add_argument("out")
    p.add_argument("--orient", choices=["l", "p", "both"], default="l")
    p.set_defaults(fn=cmd_spotlight)
    p = sub.add_parser("upload", help="upload DIR/l and DIR/p (or DIR/dl, DIR/dp with --daily) to the deck")
    p.add_argument("dir")
    p.add_argument("host")
    p.add_argument("--daily", action="store_true", help="into the daily archive (spotlight output)")
    p.add_argument("--stop", action="store_true", help="stop at the first failure")
    p.set_defaults(fn=cmd_upload)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
