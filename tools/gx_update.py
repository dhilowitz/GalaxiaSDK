#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""Install a firmware on a Galaxia over USB MIDI: no bootloader, no sudo, the same on Linux, macOS and Windows.

  gx_update.py PACKAGE.fwsc [--port NAME] [--yes] [--slow]     (tools/gx_pkg.py package makes one)
  gx_update.py Galaxia.fwsc                                    (M-VAVE's official file: back to the stock firmware)
  gx_update.py --info [--port NAME]

The running firmware checks the package and restarts into its update loader ("Galaxia Update"), which reads
the whole package, writes the app area and restarts into the new firmware. The LED is amber while the firmware
checks, then red, flickering green as the loader writes. Nothing is erased before the whole package has been
read and checked. If the install stops while the loader runs, run it again: it finishes the write.

What goes where:
  - a Galaxia SDK package, on a pedal running Galaxia SDK firmware or M-VAVE's own firmware (a Galaxia, or an
    Elemental Pro, which is the same board). M-VAVE's firmware checks the package's head, so packages made by
    gx_pkg.py before it built one are refused there (nothing is written).
  - M-VAVE's official package for the pedal (from m-vave.com/download, known here by its SHA-256), on a pedal
    running Galaxia SDK firmware: back to the stock firmware. The SDK firmware cannot tell a Galaxia from an
    Elemental Pro, so choose the file for your pedal.

This drives felucca-bsp's tools/fm1_install.py (MPL-2.0, Leo Kuroshita), the M-UPGRADE updater, with the
Galaxia's names. Needs mido and python-rtmidi (pip install mido python-rtmidi).
"""
import argparse, hashlib, os, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "bsp" / "tools"))
import fm1_install as I  # noqa: E402

MODEL = "SDK-Galaxia"
# M-VAVE's firmware on the board the SDK runs on: the models a Galaxia SDK package may be installed over
STOCK = {"Galaxia": "Galaxia", "ELEMENTAL PRO": "Elemental Pro"}
# M-VAVE's official packages (m-vave.com/download), by SHA-256: the identity the pedal reports once it runs one
# (the package's own identity marker cannot spell "Galaxia": its x encodes to the marker's "unused" value)
OFFICIAL = {
    "b26f4e5807d18e46bcb08924249d8490ebca135e370afcbc0dca47ca975a0f77": ("Galaxia_004", "Galaxia V04 (2026-11-09)"),
    "0642a11425d269b2663e0f3bc695d33773b4f20677bcdd62a474c1b1fd751c14":
        ("ELEMENTAL PRO_004", "Elemental Pro V04 (2026-11-06)"),
}


# the pedal's MIDI ports: "Galaxia <name>" (SDK firmware), "Galaxia Update" (the loader), "FM-1" (M-VAVE's firmware
# names its port after the FM-1, whose platform it shares) and "USB Composite Device" (M-VAVE's update loader).
# Only the handshake is sent while looking; nothing else goes to a device that does not answer as one of the
# models above. --port names one other port. GX_PORTS names others to look for as well (a regular expression), for
# firmwares whose USB name leaves out "Galaxia", so that an install from or to one finds the pedal before and after.
def port_re(extra=""):
    base = "galaxia|fm-1|composite"
    return re.compile(base + "|" + extra if extra else base, re.I)


I.PORT_RE = port_re(os.environ.get("GX_PORTS", ""))
# fm1_install waits 10 ms before each reply; a Galaxia from macOS needs none (an update: 33 s instead of 62)
SLOW_REPLY = I.DELAY["reply"]
I.DELAY["reply"] = 0

_parse_identity = I.parse_identity


def parse_identity(pkt):
    """M-VAVE's update loader sometimes answers the handshake with a blank identity: that is a loader too"""
    ident = _parse_identity(pkt)
    if ident is None and len(pkt) > 2 and pkt[0] == 0xF0 and pkt[-1] == 0xF7:
        d = I.unpack7(pkt[1:-1])
        if len(d) == 34 and d[:3] == b"\x00\x59\x11" and not d[6:33].strip(b"\0"):
            return I.Identity("ota-(unnamed)_0", "ota-(unnamed)", 0)
    return ident


I.parse_identity = parse_identity


class Error(Exception):
    pass


def say(e):
    """fm1_install's messages, about the pedal"""
    return str(e).replace("the FM-1", "the pedal").replace("FM-1", "Galaxia")


class Said:
    """an output stream that says fm1_install's progress lines about the pedal"""

    def __init__(self, out):
        self.out = out

    def write(self, s):
        return self.out.write(say(s))

    def flush(self):
        self.out.flush()


def load(path):
    """-> (identity the pedal reports after the install, image, what it is)"""
    raw = Path(path).read_bytes()
    official = OFFICIAL.get(hashlib.sha256(raw).hexdigest())
    if official:
        return official[0], I.logical_image(raw), "M-VAVE's official " + official[1]
    product, image = I.load_package(path, False)
    if I.model_of(product) != MODEL:
        raise Error("%s is neither a Galaxia SDK package nor an official M-VAVE package known here" % product)
    return product, image, "a Galaxia SDK package"


def refusal(ident, product):
    """why this package must not go on this pedal, or None"""
    if ident.loader:
        return None                     # an update did not finish: the loader takes the package it is given
    running, model = I.model_of(ident.text), I.model_of(product)
    if running == MODEL:
        return None
    if running in STOCK and (model == MODEL or model == running):
        return None
    if running in STOCK:
        return "the pedal runs M-VAVE's %s firmware (%s); this package is for %s" % (STOCK[running], ident.text,
                                                                                   STOCK.get(model, model))
    return "%s is not a Galaxia or an Elemental Pro; nothing sent" % ident.text


def install(up, dev, image, product, step):
    """running firmware -> loader -> new firmware, without fm1_install's own model check (refusal() is this tool's)"""
    ota = dev if dev.id.loader else up.check(dev, image, step)
    up.write(ota, image, step)
    return up.verify(product, step)


def describe(ident):
    if ident.loader:
        return "update loader: an update did not finish"
    running = I.model_of(ident.text)
    return "running" + ("" if running == MODEL else ", M-VAVE's %s firmware" % STOCK.get(running, running))


def run(a, backend, out, ask):
    up = I.Updater(backend, a.port)
    dev = up.find()
    if not dev:
        names = backend.input_names()
        raise Error("no Galaxia answered (is it plugged in and no other app using its MIDI port?)\n"
                    "  MIDI inputs: %s" % (", ".join(names) or "(none)"))
    try:
        print("pedal:   %s  [%s]  port: %s" % (dev.id.text, describe(dev.id), dev.name), file=out)
        if a.info:
            return 0
        product, image, what = load(a.package)
        print("package: %s, %s  (%s)" % (product, what, a.package), file=out)
        why = refusal(dev.id, product)
        if why:
            raise Error(why)
        if not a.yes and not ask("Install? Keep the pedal plugged in until it is done. [y/N] "):
            print("cancelled", file=out)
            return 1
    except BaseException:
        dev.link.close()
        raise
    prog = I.Progress(Said(out))
    try:
        install(up, dev, image, product, prog)
    finally:
        prog.end()
    return 0


def main(argv=None, backend=None, out=sys.stdout, ask=I.ask_tty):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package", nargs="?", help="update package (.fwsc, from gx_pkg.py, or M-VAVE's official one)")
    ap.add_argument("--info", action="store_true", help="print what the pedal runs")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    ap.add_argument("--slow", action="store_true", help="wait before each reply, as fm1_install does (if an "
                    "update stalls)")
    a = ap.parse_args(argv)
    if a.slow:
        I.DELAY["reply"] = SLOW_REPLY
    if bool(a.info) == bool(a.package):
        ap.error("give a PACKAGE.fwsc or --info")
    try:
        if backend is None:
            try:
                backend = I.MidoBackend()
            except ImportError:
                raise Error("needs mido and python-rtmidi (pip install mido python-rtmidi)")
        return run(a, backend, out, ask)
    except Error as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    except I.InstallError as e:
        msg = say(e)
        if e.code == "noloader":
            msg += "\n  (after console d, a dry run, this is expected: the firmware checked the package and carries on)"
        if e.code == "stopped" and "nothing was written" in msg:
            msg += ("\n  (M-VAVE's firmware refuses a package without a head: make it again with the current "
                    "gx_pkg.py)")
        print("error: %s" % msg, file=sys.stderr)
        return I.EXIT.get(e.code, 1)
    except KeyboardInterrupt:
        print("\ninterrupted: if the pedal shows up as \"Galaxia Update\", run the same command again to finish",
              file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
