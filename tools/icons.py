#!/usr/bin/env python3
"""Renders every PiFire app icon from the one drawing in web/icon.svg.

The mark was hand-edited once and the PNGs beside it were not re-rendered, so the phone kept
showing a drawing the repository no longer contained.  Nothing here is drawn by hand: the shapes
are lifted out of web/icon.svg and every other file -- the dark variant, the maskable variant and
all four PNGs -- is generated from them, so they cannot drift apart again.

    python3 tools/icons.py            # render
    python3 tools/icons.py --check    # fail if anything is out of date (CI, pre-release)
"""
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "web"

CHROME = next((c for c in ("google-chrome", "chromium", "chromium-browser")
               if shutil.which(c)), None)

# The face behind the mark.  Light is the warm barrel orange; dark is the black face iOS 18 asks
# for in its dark appearance, with the grill itself becoming the orange.
FACES = {
    "": ("""<linearGradient id="f" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ff9f3d"/><stop offset="0.55" stop-color="#ff7a18"/>
      <stop offset="1" stop-color="#e2540a"/></linearGradient>
    <linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.22"/>
      <stop offset="0.45" stop-color="#ffffff" stop-opacity="0"/></linearGradient>""", "#fff"),
    "dark": ("""<linearGradient id="f" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#17171a"/><stop offset="1" stop-color="#000000"/></linearGradient>
    <linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ff8a1f" stop-opacity="0.10"/>
      <stop offset="0.5" stop-color="#ff8a1f" stop-opacity="0"/></linearGradient>""", "#ff8a1f"),
}


def mark():
    """The shapes inside the <g> of web/icon.svg -- the single copy of the drawing."""
    src = (WEB / "icon.svg").read_text()
    m = re.search(r"<g transform[^>]*>(.*?)</g>", src, re.S)
    if not m:
        sys.exit("web/icon.svg: no <g> holding the mark")
    return m.group(1).strip()


def svg(shapes, face="", *, maskable=False):
    """One icon.  Maskable drops the corner rounding (the launcher supplies the shape) and pulls
    the mark in so it clears Android's 80% safe circle."""
    defs, stroke = FACES[face]
    radius = "" if maskable else ' rx="114"'
    scale = 11.0 if maskable else 13.0
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" role="img" '
            f'aria-label="PiFire">\n  <defs>\n    {defs}\n  </defs>\n'
            f'  <rect width="512" height="512"{radius} fill="url(#f)"/>\n'
            f'  <rect width="512" height="512"{radius} fill="url(#g)"/>\n'
            f'  <g transform="translate(256 256) scale({scale}) translate(-12 -12)"\n'
            f'     fill="none" stroke="{stroke}" stroke-width="1.7" stroke-linecap="round" '
            f'stroke-linejoin="round">\n    {shapes}\n  </g>\n</svg>\n')


def png(source, size, out):
    if not CHROME:
        sys.exit("no Chrome/Chromium on PATH to rasterise with")
    with tempfile.TemporaryDirectory() as tmp:
        page = pathlib.Path(tmp) / "i.html"
        page.write_text(f"<!doctype html><meta charset=utf-8><style>html,body{{margin:0;padding:0;"
                        f"background:transparent}}svg{{display:block;width:{size}px;"
                        f"height:{size}px}}</style>\n{source}")
        subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        "--no-sandbox", f"--user-data-dir={tmp}/u",
                        "--default-background-color=00000000",
                        "--force-device-scale-factor=1", f"--window-size={size},{size}",
                        f"--screenshot={out}", page.as_uri()],
                       check=True, capture_output=True)


def main():
    check = "--check" in sys.argv
    shapes = mark()
    light, dark = svg(shapes), svg(shapes, "dark")
    wrote = []

    def put(path, data):
        p = WEB / path
        old = p.read_bytes() if p.exists() else None
        new = data if isinstance(data, bytes) else data.encode()
        if old == new:
            return
        wrote.append(path)
        if not check:
            p.write_bytes(new)

    put("icon-dark.svg", dark)

    jobs = [("icon-180.png", light, 180), ("icon-192.png", light, 192),
            ("icon-512.png", light, 512), ("icon-180-dark.png", dark, 180),
            ("icon-192-dark.png", dark, 192), ("icon-512-dark.png", dark, 512),
            ("icon-512-maskable.png", svg(shapes, maskable=True), 512),
            ("icon-512-maskable-dark.png", svg(shapes, "dark", maskable=True), 512)]
    with tempfile.TemporaryDirectory() as tmp:
        for name, source, size in jobs:
            out = pathlib.Path(tmp) / name
            png(source, size, out)
            put(name, out.read_bytes())

    if check and wrote:
        sys.exit("icons are out of date, run tools/icons.py: " + ", ".join(wrote))
    print("icons up to date" if not wrote else "rendered: " + ", ".join(wrote))


if __name__ == "__main__":
    main()
