#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""Install a firmware on a Galaxia over USB MIDI, while a Galaxia SDK firmware runs on it: no bootloader, no
sudo, the same on Linux, macOS and Windows.

  gx_update.py PACKAGE.fwsc [--port NAME] [--yes]     (tools/gx_pkg.py package makes PACKAGE.fwsc)
  gx_update.py --info [--port NAME]

The running firmware checks the package and restarts into its update loader ("Galaxia Update"), which reads
the whole package, writes the app area and restarts into the new firmware. The LED is amber while the firmware
checks, then red, flickering green as the loader writes. Nothing is erased before the whole package has been
read and checked. If the install stops while the loader runs, run it again: it finishes the write.

This drives felucca-bsp's tools/fm1_install.py (MPL-2.0, Leo Kuroshita), the M-UPGRADE updater, with the
Galaxia's names. Needs mido and python-rtmidi (pip install mido python-rtmidi).

Only Galaxia SDK firmware ("SDK-Galaxia_N") is updated this way. A pedal still on M-VAVE's firmware
("Galaxia_002") is never sent the upgrade command: install the first SDK firmware with gx_flash.py.
"""
import argparse, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "bsp" / "tools"))
import fm1_install as I  # noqa: E402

MODEL = "SDK-Galaxia"
# the pedal's MIDI ports: "Galaxia <name>" (SDK firmware), "Galaxia Update" (the loader); never other gear
I.PORT_RE = re.compile(r"galaxia", re.I)


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


def run(a, backend, out, ask):
    up = I.Updater(backend, a.port)
    dev = up.find()
    if not dev:
        names = backend.input_names()
        raise Error("no Galaxia answered (is it plugged in, running an SDK firmware, and no other app using its "
                    "MIDI port?)\n  MIDI inputs: %s" % (", ".join(names) or "(none)"))
    try:
        mode = "update loader: an update did not finish" if dev.id.loader else "running"
        print("pedal:   %s  [%s]  port: %s" % (dev.id.text, mode, dev.name), file=out)
        if I.model_of(dev.id.text) != MODEL:
            raise Error("%s is not a Galaxia SDK firmware; nothing sent (a stock pedal: install with "
                        "gx_flash.py first)" % dev.id.text)
        if a.info:
            return 0
        product, image = I.load_package(a.package, False)
        print("package: %s  (%s)" % (product, a.package), file=out)
        if I.model_of(product) != MODEL:
            raise Error("%s is not a Galaxia SDK package" % product)
        if not a.yes and not ask("Install? Keep the pedal plugged in until it is done. [y/N] "):
            print("cancelled", file=out)
            return 1
    except BaseException:
        dev.link.close()
        raise
    prog = I.Progress(Said(out))
    try:
        up.install(dev, image, product, prog)
    finally:
        prog.end()
    return 0


def main(argv=None, backend=None, out=sys.stdout, ask=I.ask_tty):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package", nargs="?", help="update package (.fwsc, from gx_pkg.py)")
    ap.add_argument("--info", action="store_true", help="print what the pedal runs")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    a = ap.parse_args(argv)
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
        print("error: %s" % msg, file=sys.stderr)
        return I.EXIT.get(e.code, 1)
    except KeyboardInterrupt:
        print("\ninterrupted: if the pedal shows up as \"Galaxia Update\", run the same command again to finish",
              file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
