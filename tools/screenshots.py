#!/usr/bin/env python3
"""Capture landing-page screenshots of the app running in demo mode.

Uses headless Google Chrome (set $CHROME to override the binary).  The app is
loaded straight from disk: sensor.js embeds the wasm, so no server is needed.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

CANDIDATES = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "google-chrome",
    "google-chrome-stable",
    "chromium",
]

# (output name, window size, URL query)
SHOTS = [
    ("app-demo.png", (1400, 860), "?demo"),
]


def find_chrome():
    for c in [os.environ.get("CHROME")] + CANDIDATES:
        if c and (os.path.exists(c) or shutil.which(c)):
            return c
    sys.exit("Google Chrome not found; set $CHROME")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--app", required=True, help="path to the app index.html")
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args()

    chrome = find_chrome()
    app = pathlib.Path(args.app).resolve()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for name, (w, h), query in SHOTS:
        dest = (out / name).resolve()
        subprocess.run([
            chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
            f"--window-size={w},{h}",
            # Let the demo run long enough to fill the FFT and pressure plots.
            "--virtual-time-budget=6000",
            f"--screenshot={dest}",
            app.as_uri() + query,
        ], check=True, stderr=subprocess.DEVNULL)
        print(f"wrote {dest}")


if __name__ == "__main__":
    main()
