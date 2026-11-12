#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""Make an installer page with firmwares built in: docs/installer, its packages and their licences, in one folder.

  gx_install_page.py OUT_DIR --firmware NAME PACKAGE.fwsc [DESCRIPTION] [--firmware ...]
                     [--title TEXT] [--intro TEXT] [--find NAME ...] [--no-own-package] [--port N]

OUT_DIR gets index.html, gx_update.js, the packages (firmware/), the licence texts their JieLi files need
(licences/) and serve.command. The page lists the firmwares, offers the way back to M-VAVE's firmware (with the
official file the user downloads) and, unless --no-own-package, any package the user has. It checks each package's
size and SHA-256 when it loads.

Browsers only allow Web MIDI over https or localhost: put the folder on an https site, or double-click
serve.command (macOS), which serves it on localhost and opens Chrome. --find adds part of a USB name to look for
as well as "Galaxia", for a firmware named some other way.
"""
import argparse, hashlib, json, re, shutil, stat, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "bsp" / "tools"))
import fm1_install as I  # noqa: E402

MARK = '<script type="application/json" id="builtin">null</script>'
SERVE = """#!/bin/sh
# Serves this folder on localhost and opens Chrome. Close this window to stop. The server tells the browser not to
# keep copies, so another page served on the same port later is never shown from its cache.
cd "$(dirname "$0")" || exit 1
python3 -c '
import http.server, sys
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
http.server.ThreadingHTTPServer(("127.0.0.1", {port}), H).serve_forever()
' &
server=$!
trap 'kill $server 2>/dev/null' EXIT
sleep 1
kill -0 $server 2>/dev/null || {{ echo "Couldn't start the server. Is port {port} already in use?"; exit 1; }}
open -a "Google Chrome" "http://localhost:{port}/"
wait $server
"""
LICENCES = """<!doctype html><meta charset="utf-8"><title>Licences</title>
<h1>Licences of the files in these firmware packages</h1>
<ul>
<li>The Galaxia SDK runtime and update loader, and felucca-bsp's chip layer: Mozilla Public License 2.0
(<a href="LICENSE.txt">LICENSE.txt</a>, <a href="NOTICE.txt">NOTICE.txt</a>). Source: <a
href="https://github.com/dhilowitz/GalaxiaSDK">github.com/dhilowitz/GalaxiaSDK</a>.</li>
<li>Each package also holds two files from JieLi's AC79 SDK, <code>uboot.boot</code> and <code>cfg_tool.bin</code>:
Apache License 2.0 (<a href="Apache-2.0.txt">Apache-2.0.txt</a>).</li>
<li>Each firmware's own code is under the licence its author chose.</li>
</ul>
"""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path, help="folder to write the page into")
    ap.add_argument("--firmware", nargs="+", action="append", default=[], metavar="ARG",
                    help="NAME PACKAGE.fwsc [DESCRIPTION]; repeat for more")
    ap.add_argument("--title", help='the title of the page (default "Galaxia Installer")')
    ap.add_argument("--intro", help="the line under the title")
    ap.add_argument("--find", action="append", default=[], metavar="NAME",
                    help='part of a USB name to look for as well as "Galaxia"; repeat for more')
    ap.add_argument("--no-own-package", action="store_true", help="no choice for a package of the user's own")
    ap.add_argument("--port", type=int, default=8765, help="localhost port for serve.command (default 8765); give each page its own")
    a = ap.parse_args(argv)

    html = (ROOT / "docs" / "installer" / "index.html").read_text(encoding="utf-8")
    if html.count(MARK) != 1:
        raise SystemExit("docs/installer/index.html must hold the builtin block once; update this tool")
    fw_dir = a.out / "firmware"
    fw_dir.mkdir(parents=True, exist_ok=True)
    for old in fw_dir.glob("*.fwsc"):
        old.unlink()
    firmware = []
    for args in a.firmware:
        if not 2 <= len(args) <= 3:
            raise SystemExit("--firmware takes NAME PACKAGE.fwsc [DESCRIPTION]")
        name, path = args[0], Path(args[1])
        raw = path.read_bytes()
        product = I.product_of(raw)
        if I.model_of(product) != "SDK-Galaxia" or I.LOADER_MARK not in raw:
            raise SystemExit("%s is not a Galaxia SDK package" % path)
        file = re.sub(r"[^A-Za-z0-9.-]+", "-", name).strip("-").lower() + ".fwsc"
        (fw_dir / file).write_bytes(raw)
        firmware.append({"name": name, "description": args[2] if len(args) > 2 else "", "file": "firmware/" + file,
                         "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
        print("%s: %s, %d bytes, sha256 %s" % (name, product, len(raw), firmware[-1]["sha256"][:12]))
    meta = {"firmware": firmware, "ports": a.find, "licences": "licences/"}
    if a.title:
        meta["title"] = a.title
    if a.intro:
        meta["intro"] = a.intro
    if a.no_own_package:
        meta["ownPackage"] = False
    block = MARK.replace("null", json.dumps(meta).replace("<", "\\u003c"))
    (a.out / "index.html").write_text(html.replace(MARK, block), encoding="utf-8")
    shutil.copy(ROOT / "docs" / "installer" / "gx_update.js", a.out / "gx_update.js")
    lic = a.out / "licences"
    lic.mkdir(exist_ok=True)
    shutil.copy(ROOT / "LICENSE", lic / "LICENSE.txt")
    shutil.copy(ROOT / "NOTICE", lic / "NOTICE.txt")
    shutil.copy(ROOT / "bsp" / "LICENSES" / "Apache-2.0.txt", lic / "Apache-2.0.txt")
    (lic / "index.html").write_text(LICENCES, encoding="utf-8")
    serve = a.out / "serve.command"
    serve.write_text(SERVE.format(port=a.port), encoding="utf-8")
    serve.chmod(serve.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    print("%s: %d firmware(s)" % (a.out, len(firmware)))


if __name__ == "__main__":
    main()
