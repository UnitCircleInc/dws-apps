#!/usr/bin/env python3
"""Assemble the deployable site from web/ into an output directory.

  <out>/index.html                          hub page listing the tools
  <out>/assets/                             shared styles and icons
  <out>/<tool>/index.html                   tool landing page
  <out>/<tool>/app/                         hosted app
  <out>/<tool>/downloads/dws-<tool>.html    standalone single-file app

A tool is any web/<tool>/ with an app/index.html.  Every page gets
{{VERSION}} / {{COMMIT}} / {{BUILD_DATE}} substituted, and its local .css/.js
references get a ?v=<content hash> so browsers never pair a new page with a
stale cached stylesheet or script.  The standalone file
inlines the app's local scripts (sensor.js already embeds the wasm) and the
favicon, so it runs when opened straight from disk in Chrome or Edge.
"""

import argparse
import base64
import datetime
import hashlib
import pathlib
import re
import shutil
import subprocess

SITE_URL = "https://dws-apps.unitcircle.ca/"
KISSFFT_LICENSE = pathlib.Path("vendor/kissfft/LICENSES/BSD-3-Clause")


def git(*args):
    try:
        return subprocess.run(["git", *args], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def solover():
    """Version string per SoloVer (https://beza1e1.tuxen.de/SoloVer).

    Release tags are bare numbers ("1", "2", ...).  A build on a tag is that
    number; later commits get a "+" postfix, which sorts after the release:
    "1+2_gabc1234" (2 commits past 1), with "_dirty" for uncommitted changes.
    """
    desc = git("describe", "--tags", "--long", "--dirty", "--match", "[0-9]*")
    m = re.fullmatch(r"(\d+)-(\d+)-(g[0-9a-f]+)(-dirty)?", desc)
    if not m:
        sha = git("rev-parse", "--short", "HEAD")
        if not sha:
            return "0"
        dirty = "_dirty" if git("status", "--porcelain", "--untracked-files=no") else ""
        return f"0+g{sha}{dirty}"
    number, ahead, sha, dirty = m.groups()
    post = []
    if ahead != "0":
        post += [ahead, sha]
    if dirty:
        post.append("dirty")
    return number + ("+" + "_".join(post) if post else "")


def stamp(text, info):
    for key, val in info.items():
        text = text.replace("{{" + key + "}}", val)
    return text


def cache_bust(html, page):
    """Append ?v=<hash of file contents> to relative .css/.js references."""
    def add_hash(m):
        target = page.parent / m.group(2)
        if not target.is_file():
            return m.group(0)
        digest = hashlib.sha256(target.read_bytes()).hexdigest()[:8]
        return f'{m.group(1)}="{m.group(2)}?v={digest}"'
    return re.sub(r'\b(href|src)="([^":?#]+\.(?:css|js))"', add_hash, html)


def inline_script(js):
    # A literal "</script" inside inlined JS would end the element early.
    return "<script>\n" + js.replace("</script", "<\\/script") + "\n</script>"


def build_standalone(src, tool, info):
    app_dir = src / tool / "app"
    html = (app_dir / "index.html").read_text()

    def inline(m):
        return inline_script((app_dir / m.group(1)).read_text())
    html = re.sub(r'<script src="([^":/]+\.js)"></script>', inline, html)

    icon = (src / "assets" / "favicon.svg").read_bytes()
    icon_uri = "data:image/svg+xml;base64," + base64.b64encode(icon).decode()
    html = re.sub(r'href="(\.\./)+assets/favicon\.svg"', f'href="{icon_uri}"', html)

    title = re.search(r"<title>(.*?)</title>", html).group(1)
    notice = (
        "<!--\n"
        f"  {title} {info['VERSION']} ({info['COMMIT']}, {info['BUILD_DATE']})\n"
        f"  {SITE_URL}{tool}/\n\n"
        "  Standalone build: open this file in Chrome or Edge.\n\n"
        "  Copyright Unit Circle Inc., licensed under the Apache License 2.0.\n\n"
        "  Includes KISS FFT, Copyright (c) 2003-2010 Mark Borgerding,\n"
        "  used under the BSD 3-Clause licence:\n\n"
        + KISSFFT_LICENSE.read_text().replace("--", "- -")
        + "\n-->\n"
    )
    html = re.sub(r"(<!DOCTYPE html>\n)", lambda m: m.group(1) + notice, html,
                  count=1, flags=re.IGNORECASE)
    return stamp(html, info)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", required=True, type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    args = ap.parse_args()

    info = {
        "VERSION": solover(),
        "COMMIT": git("rev-parse", "--short", "HEAD") or "unknown",
        "BUILD_DATE": datetime.date.today().isoformat(),
    }

    if args.out.exists():
        shutil.rmtree(args.out)
    shutil.copytree(args.src, args.out)

    for page in args.out.rglob("*.html"):
        page.write_text(cache_bust(stamp(page.read_text(), info), page))

    print(f"built {args.out}/ ({info['VERSION']})")
    for app_index in sorted(args.src.glob("*/app/index.html")):
        tool = app_index.parent.parent.name
        downloads = args.out / tool / "downloads"
        downloads.mkdir(exist_ok=True)
        standalone = downloads / f"dws-{tool}.html"
        standalone.write_text(build_standalone(args.src, tool, info))
        print(f"  {standalone} ({standalone.stat().st_size // 1024} KiB)")


if __name__ == "__main__":
    main()
