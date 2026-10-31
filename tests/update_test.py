#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""tools/gx_update.py against a simulated pedal (no hardware, no mido): the running firmware's check, the
update loader's write, the restart; and that a stock pedal is never sent the upgrade command.
  python3 tests/update_test.py PACKAGE.fwsc          (tools/gx_pkg.py package makes one)"""
import io, queue, sys, threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gx_update as G  # noqa: E402

I = G.I
I.DELAY.update(open=0, start=0.01, reply=0, loader=0.01, reboot=0.01, retry=0.02, poll=0.05, hs=0.1,
               idle_check=0.4, idle_write=0.4, wait_loader=2, wait_reboot=2)
failed = 0


def ok(cond, what):
    global failed
    print("%-64s %s" % (what, "ok" if cond else "FAIL"))
    failed += not cond


class Link:
    def __init__(self, dev, gen):
        self.dev, self.gen, self.q, self.closed = dev, gen, queue.Queue(), False

    @property
    def lost(self):
        return self.gen != self.dev.gen

    def send(self, pkt):
        if self.lost:
            return False
        self.dev.rx(bytes(pkt))
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        self.closed = True


class Pedal:
    """a MIDI backend with one pedal on it; reads the package the way the firmware and the loader do"""

    def __init__(self, image, identity, name):
        self.image, self.upgrades, self.served, self.bad = image, 0, 0, 0
        self.links, self.gen = [], 0
        self.boot(identity, name)

    def boot(self, identity, name):
        self.gen += 1
        self.identity, self.name, self.waiting, self.queue = identity, name, None, []

    def input_names(self):
        return [self.name]

    output_names = input_names

    def open(self, i, o):
        link = Link(self, self.gen)
        self.links.append(link)
        return link

    def tx(self, pkt):
        for link in self.links:
            if link.gen == self.gen and not link.closed:
                link.q.put(pkt)

    def rx(self, d):
        if d == I.HS_QUERY:
            t = self.identity.encode()
            self.tx(b"\xF0" + I.pack7(bytes([0, 0x59, 0x11, 0, 0, 0]) + t + bytes(28 - len(t))) + b"\xF7")
        elif d == I.UPGRADE:
            self.upgrades += 1
            if self.identity.startswith("ota-"):            # the loader: the whole flash.bin, then finish
                self.queue = [(k, 512) for k in range(0x400, 0x400 + 0x4000, 512)] + [(I.FINISH_WRITE, 8)]
            else:                                           # the firmware: the header and the loader, then go
                self.queue = [(0, 512), (512, 512), (0x1000, 512), (I.FINISH_CHECK, 8)]
            self.next()
        elif self.waiting:
            u = I.unpack7(d[1:-1])
            addr, n = self.waiting
            want = b"success\0" if addr >= I.FINISH_CHECK else self.image[addr:addr + n]
            self.bad += u[14:14 + len(want)] != want
            self.served += 1
            self.waiting = None
            if addr == I.FINISH_CHECK:
                threading.Timer(0.05, self.boot, ("ota-SDK-Galaxia_900", "Galaxia Update")).start()
            elif addr == I.FINISH_WRITE:
                threading.Timer(0.05, self.boot, ("SDK-Galaxia_900", "Galaxia Reverb")).start()
            else:
                self.next()

    def next(self):
        if self.queue:
            self.waiting = addr, n = self.queue.pop(0)
            u = bytearray([0, 0x59, 0x30, 0, 0, 0, 0]) + addr.to_bytes(4, "little") + bytes([n & 0xFF, n >> 8, 0])
            u.append(~sum(u[6:14]) & 0xFF)
            self.tx(b"\xF0" + I.pack7(u) + b"\xF7")


def run(pedal, *args):
    out = io.StringIO()
    rc = G.main(list(args), backend=pedal, out=out, ask=lambda p: True)
    return rc, out.getvalue()


def main():
    pkg = sys.argv[1]
    _, image = I.load_package(pkg, False)

    p = Pedal(image, "SDK-Galaxia_900", "Galaxia Reverb")
    rc, out = run(p, pkg)
    ok(rc == 0 and "done: the pedal runs SDK-Galaxia_900" in out, "update: firmware check, loader write, restart")
    ok(p.upgrades == 2 and p.bad == 0 and p.served > 30, "both steps asked for the package and got it right")

    p = Pedal(image, "ota-SDK-Galaxia_900", "Galaxia Update")
    rc, out = run(p, pkg)
    ok(rc == 0 and p.upgrades == 1, "a pedal left in the loader: the write is finished")

    p = Pedal(image, "Galaxia_002", "FM-1")
    rc, _ = run(p, pkg)
    ok(rc != 0 and p.upgrades == 0, "stock firmware (MIDI port FM-1): not found, nothing sent")

    p = Pedal(image, "Galaxia_002", "Galaxia")
    rc, _ = run(p, pkg)
    ok(rc != 0 and p.upgrades == 0, "stock firmware on a Galaxia-named port: refused, nothing sent")

    p = Pedal(image, "SDK-Galaxia_900", "Galaxia Reverb")
    rc, out = run(p, "--info")
    ok(rc == 0 and "SDK-Galaxia_900" in out and p.upgrades == 0, "--info: reports, sends nothing")
    print("FAILED: %d" % failed if failed else "all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
