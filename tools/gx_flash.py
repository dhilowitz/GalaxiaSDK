#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
# The loader upload follows MvaveFM1Unbricker's fm1_unbrick.py (MIT, Copyright (c) 2026 Michael P Jones),
# which follows kagaimiq's jl-uboot-tool (MIT, Copyright (c) 2023 Andrey Grigoryev).
"""Back up, install and restore Galaxia firmware through the chip's ROM USB bootloader.

The pedal must be in the bootloader ("WL82 UBOOT1.00" over USB): run `gx_softkey.py` while the stock
firmware or a Galaxia SDK firmware runs, or let a broken build drop there by itself (two failed starts).
Linux only for now (jl-uboot-tool's SCSI access; run as a user allowed to open the /dev/sg device).

  gx_flash.py backup BACKUP.bin            the whole 1 MiB flash, read twice and compared. Do this first:
                                           it is the only copy of your pedal's stock firmware.
  gx_flash.py install APP.bin BACKUP.bin   build an image from APP.bin (build/<example>/<example>.bin)
                                           with your backup's boot head, write it, verify it, restart
  gx_flash.py restore BACKUP.bin           put the backed-up firmware back, verify it, restart
  gx_flash.py run                          restart the firmware already on the pedal (nothing written)

The boot head (0x0000-0x3FFF: the boot loader and its settings) and the factory key area
(0xFF000-0xFFFFF) are never erased or written.
"""
import argparse, hashlib, os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
JLUB = HERE / "jl-uboot-tool"
sys.path.insert(0, str(JLUB))
sys.path.insert(0, str(HERE))
import jl_package  # noqa: E402

FLASH_SIZE = 0x100000
HEAD_END, KEY_MAC = 0x4000, 0xFF000
SECTOR, BLOCK, PAGE = 0x1000, 0x10000, 0x100
MAILBOX, MAILBOX_LEN = 0x01C7FD80, 0x80          # boot request and update record (RAM)
CHIP_KEY, FLASH_ID = 0x980F, 0x856014           # every Galaxia (and FM-1) seen so far
WL82_LOADER_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def find_uboot():
    """/dev/sgN of a USB device answering as WL82 UBOOT1.00."""
    base = Path("/sys/class/scsi_generic")
    found = []
    for e in sorted(base.iterdir()) if base.is_dir() else []:
        try:
            vendor = (e / "device" / "vendor").read_text().strip()
            model = (e / "device" / "model").read_text().strip()
        except OSError:
            continue
        if vendor.upper() == "WL82" and model.upper().startswith("UBOOT1.00") and "/usb" in os.path.realpath(e / "device"):
            found.append("/dev/" + e.name)
    if len(found) != 1:
        raise SystemExit("need exactly one pedal in the bootloader (WL82 UBOOT1.00), found %d" % len(found))
    return found[0]


class Pedal:
    """The pedal in UBOOT with jl-uboot-tool's USB loader running."""

    def __init__(self):
        import yaml
        from jltech.uboot import JL_UBOOT, JL_LoaderV2
        from jltech.cipher import cipher_bytes, jl_crc_cipher
        from scsiio import SCSIDev
        self.scsi = SCSIDev(find_uboot())
        self.ldr = JL_LoaderV2(self.scsi)
        try:                                    # a loader left running by an earlier session
            self.ldr.chip_key()
            return
        except Exception:
            pass
        data = JLUB / "data"
        spec = yaml.safe_load((data / "usb-loaders.yaml").read_text())["wl82"]
        chip = yaml.safe_load((data / "chips.yaml").read_text())["wl82"]
        blob = (data / spec["file"]).read_bytes()
        if sha(blob) != WL82_LOADER_SHA256:
            raise SystemExit("unexpected wl82 loader in jl-uboot-tool; refusing")
        cipher = spec.get("encryption", "none")
        quirks = (chip.get("uboot1.00") or {}).get("quirks") or {}
        rom_crypt = quirks.get("memory-rw-mengli-crypt") is True
        crypt = (not rom_crypt and cipher == "MengLi") or (rom_crypt and cipher != "MengLi")
        uboot = JL_UBOOT(self.scsi)
        bs = spec.get("blocksize", 512)
        for off in range(0, len(blob), bs):
            block = blob[off:off + bs]
            uboot.mem_write(spec["address"] + off, cipher_bytes(jl_crc_cipher, block) if crypt else block)
        uboot.mem_jump(spec["address"], 1)
        self.ldr = JL_LoaderV2(self.scsi)

    def check(self):
        key, fid = self.ldr.chip_key(), self.ldr.online_device()["id"]
        print("chip key 0x%04X, flash id 0x%06X" % (key, fid))
        if key != CHIP_KEY or fid != FLASH_ID:
            raise SystemExit("not the chip key / flash this tool knows; nothing written")
        try:
            self.io = max(16, min(int(self.ldr.usb_buffer_size()), 0x8000))
        except Exception:
            self.io = 512

    def read(self, addr, n):
        out = bytearray()
        while n:
            k = min(n, self.io)
            d = self.ldr.flash_read(addr, k)
            if len(d) != k:
                raise SystemExit("short read at 0x%X" % addr)
            out += d
            addr += k
            n -= k
        return bytes(out)

    def read_twice(self):
        a = self.read(0, FLASH_SIZE)
        b = self.read(0, FLASH_SIZE)
        if a != b:
            raise SystemExit("two reads of the flash differ: try another cable or port; nothing written")
        return a

    def program(self, image, end):
        """Make flash [HEAD_END, end) equal to image[HEAD_END:end]; verify."""
        def gate(a, n):
            if n <= 0 or a < HEAD_END or a + n > KEY_MAC:
                raise SystemExit("refusing to touch 0x%X+0x%X" % (a, n))
        a = HEAD_END
        while a < end:
            if a % BLOCK == 0 and end - a >= BLOCK:
                gate(a, BLOCK); self.ldr.flash_erase_block(a); a += BLOCK
            else:
                gate(a, SECTOR); self.ldr.flash_erase_sector(a); a += SECTOR
        pages = 0
        for off in range(HEAD_END, end, PAGE):
            chunk = image[off:off + PAGE]
            if chunk != b"\xff" * len(chunk):
                gate(off, len(chunk)); self.ldr.flash_write(off, chunk); pages += 1
        back = self.read(HEAD_END, end - HEAD_END)
        if back != image[HEAD_END:end]:
            raise SystemExit("VERIFY FAILED: do not power off; run the same command again")
        print("wrote and verified 0x%X..0x%X (%d pages)" % (HEAD_END, end - 1, pages))

    def restart(self):
        # The stock firmware's soft key leaves "usb_update_mode" at 0x01C7FD80, and the boot loader
        # would obey it once more on this restart: clear that mailbox first (as the runtime does).
        try:
            self.ldr.mem_write(MAILBOX, bytes(MAILBOX_LEN))
        except Exception:
            pass
        try:
            self.ldr.run_app()
        except Exception:
            pass                                # the chip resets before the reply
        print("restarting")


def need_modules():
    missing = []
    for m in ("yaml", "crcmod"):
        try:
            __import__(m)
        except ImportError:
            missing.append(m)
    if missing:
        raise SystemExit("gx_flash.py needs the Python modules pyyaml and crcmod (missing: %s). Debian/Ubuntu: "
                         "sudo apt install python3-yaml python3-crcmod; elsewhere: pip install pyyaml crcmod"
                         % ", ".join(missing))


def main():
    need_modules()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup"); b.add_argument("out")
    i = sub.add_parser("install"); i.add_argument("app"); i.add_argument("backup")
    r = sub.add_parser("restore"); r.add_argument("backup")
    sub.add_parser("run")
    a = ap.parse_args()

    if a.cmd == "backup" and Path(a.out).exists():
        raise SystemExit("%s exists; not overwriting a backup" % a.out)
    if a.cmd in ("install", "restore"):
        stock = Path(a.backup).read_bytes()
        if len(stock) != FLASH_SIZE:
            raise SystemExit("the backup must be the 1 MiB file gx_flash.py backup wrote")

    p = Pedal()
    p.check()
    if a.cmd == "run":
        p.restart()
        return
    if a.cmd == "backup":
        flash = p.read_twice()
        Path(a.out).write_bytes(flash)
        print("backup %s: sha256 %s" % (a.out, sha(flash)))
        names = [r[1] for r in jl_package.parse(flash)[0] if r[6]]
        print("firmware found: " + ", ".join(n for n in names if n in ("app.bin", "cfg_tool.bin")))
        p.restart()
        return
    before = p.read_twice()
    if before[:HEAD_END] != stock[:HEAD_END]:
        raise SystemExit("this pedal's boot head differs from the backup's: wrong backup? nothing written")
    if a.cmd == "install":
        image = jl_package.image_from_backup(Path(a.app).read_bytes(), stock)
        p.program(image, 0x7C000)               # the app area, and the settings areas after it erased
    else:
        p.program(stock, KEY_MAC)
    p.restart()


if __name__ == "__main__":
    main()
