#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""Send the soft key (SysEx F0 22 24 35 7D F7): the Galaxia restarts into its ROM USB bootloader.

Works with the stock firmware (which shows up over USB-MIDI as "FM-1", vendor 4c4a) and with Galaxia SDK
firmware (vendor 1209). Linux: finds the pedal's raw MIDI device by its USB id (or give --port /dev/snd/midiCxD0).
Elsewhere, with python-rtmidi installed: --port takes part of the MIDI port's name.

  gx_softkey.py              then, within a few seconds, the pedal shows up as WL82 UBOOT1.00
"""
import argparse, glob, os, sys, time

SOFT_KEY = bytes.fromhex("F0 22 24 35 7D F7")
USB_IDS = ("4c4a:c755", "1209:")             # stock Galaxia, Galaxia SDK firmware


def linux_port():
    for m in sorted(glob.glob("/dev/snd/midiC*D0")):
        card = m[len("/dev/snd/midiC"):-2]
        try:
            usbid = open("/proc/asound/card%s/usbid" % card).read().strip().lower()
        except OSError:
            continue
        if usbid.startswith(USB_IDS):
            return m
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    a = ap.parse_args()
    port = a.port
    if sys.platform.startswith("linux") and (port is None or port.startswith("/dev/")):
        port = port or linux_port()
        if not port:
            raise SystemExit("no Galaxia MIDI port found (is it plugged in and running?)")
        fd = os.open(port, os.O_WRONLY)
        os.write(fd, SOFT_KEY)
        os.close(fd)
    else:
        import rtmidi
        out = rtmidi.MidiOut()
        names = out.get_ports()
        want = (port or "FM-1").lower()
        hits = [i for i, n in enumerate(names) if want in n.lower() or "galaxia" in n.lower()]
        if not hits:
            raise SystemExit("no Galaxia MIDI port (is the pedal plugged in and running, not already in its "
                             "bootloader?); ports: %s" % names)
        out.open_port(hits[0])
        out.send_message(list(SOFT_KEY))
        time.sleep(0.2)
        port = names[hits[0]]
    print("soft key sent to %s" % port)


if __name__ == "__main__":
    main()
