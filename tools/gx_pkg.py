#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""Update packages for the Galaxia: what gx_update.py installs over USB MIDI.

  gx_pkg.py loader LOADER.bin OTA.bin
      wrap the update loader (build/loader/loader.bin, a RAM image for 0x01C0A800) as the boot loader loads
      it: a file head, a load head, then blocks of at most 4 KiB, each LZ4-compressed (build.sh does this)
  gx_pkg.py package APP.bin OUT.fwsc --cfg-tool-from BACKUP.bin | --cfg-tool CFG_TOOL.bin [--ota OTA.bin]
      a package (M-UPGRADE .fwsc, the format of M-VAVE's own updates): the app area as gx_flash.py install
      writes it, with build/loader/ota.bin as its update loader. Its identity ("SDK-Galaxia_900") is read from the
      app (GX_UPDATE_ID). cfg_tool.bin is JieLi's SDK file (Apache-2.0), the same on every Galaxia seen: give a
      backup to take it from, or the SDK's cfg_tool.bin.

The package's head (flash 0x0000-0x3FFF) is left blank: the Galaxia's update loader never writes the head and
does not read it. The UFW container and the identity marker come from felucca-bsp's tools/fm1pkg.py (MPL-2.0).
"""
import argparse, hashlib, re, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "bsp" / "tools"))
import jl_package  # noqa: E402
import fm1pkg  # noqa: E402

LOADER_LOAD = 0x01C0A800
LOADER_MAX = 0x14000                    # the RAM the loader may use (bsp/loader/loader.ld)
OTA_MAX = 0x5000 - 0x200 + 0x20         # what the firmware stages (bsp/chip/ota.c: OTA_AREA_LEN - 0x200, + head)
LOADER_NAME = b"usb_hid_ota.bin"        # the boot loader's name for the update loader
LOADER_MARK = b"FELUCCA-LOADER-1"       # bsp/loader/crt0_ldr.S: the firmware knows the loader by it
PKG_FLASH = 0x93000                     # flash.bin up to the end of what the loader writes
CFG_TOOL_SHA256 = "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b"
ID_RE = re.compile(rb"SDK-Galaxia_\d+\0")


# ---- LZ4 block format (github.com/lz4/lz4, doc/lz4_Block_format.md): a greedy compressor and a decoder

def lz4_compress(src):
    n, out, table = len(src), bytearray(), {}
    anchor, i = 0, 0
    limit = n - 12                      # the last match starts at least 12 bytes before the end

    def length(v):
        while v >= 255:
            out.append(255)
            v -= 255
        out.append(v)

    while i < limit:
        key = src[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is None or i - cand > 0xFFFF:
            i += 1
            continue
        m = 4                           # extend, leaving the last 5 bytes as literals
        while i + m < n - 5 and src[cand + m] == src[i + m]:
            m += 1
        lit = i - anchor
        out.append((min(lit, 15) << 4) | min(m - 4, 15))
        if lit >= 15:
            length(lit - 15)
        out += src[anchor:i]
        out += struct.pack("<H", i - cand)
        if m - 4 >= 15:
            length(m - 4 - 15)
        i += m
        anchor = i
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        length(lit - 15)
    out += src[anchor:]
    return bytes(out)


def lz4_decompress(src):
    out, i = bytearray(), 0
    while i < len(src):
        tok = src[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        m = (tok & 15) + 4
        if tok & 15 == 15:
            while True:
                b = src[i]
                i += 1
                m += b
                if b != 255:
                    break
        for _ in range(m):
            out.append(out[-off])
    return bytes(out)


# ---- the update loader as the boot loader loads it

def head(dcrc, a, b, attr):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, LOADER_NAME)
    return struct.pack("<H", jl_package.crc16(body)) + body


def wrap_loader(image):
    if not image or len(image) > LOADER_MAX:
        raise SystemExit("loader: %d bytes, more than its 0x%X of RAM" % (len(image), LOADER_MAX))
    if LOADER_MARK not in image[:64]:
        raise SystemExit("loader: no %s marker at its start" % LOADER_MARK.decode())
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4_compress(raw)
        if len(c) > 4096 or lz4_decompress(c) != raw:
            raise SystemExit("loader: block %d does not compress to 4096 bytes or less" % (i // 4096))
        blocks += struct.pack("<II", len(c), len(raw)) + c
    body = head(jl_package.crc16(image), len(image), LOADER_LOAD, 0) + bytes(blocks)
    ota = head(jl_package.crc16(body), 0x20, len(body), 0x41) + body
    if len(ota) > OTA_MAX:
        raise SystemExit("loader: ota.bin is %d bytes; the firmware stages at most %d" % (len(ota), OTA_MAX))
    return ota


# ---- the package

def identity(app):
    found = set(m.group()[:-1].decode() for m in ID_RE.finditer(app))
    if len(found) != 1:
        raise SystemExit("the app must contain exactly one update identity (GX_UPDATE_ID, \"SDK-Galaxia_N\"), "
                         "found %s" % (sorted(found) or "none"))
    return found.pop()


def flash_bin(app, cfg_tool):
    img, _ = jl_package.build(app, cfg_tool, b"\xff" * jl_package.AREA)
    if any(b != 0xFF for b in img[0x7C000:]):
        raise SystemExit("the app area runs past 0x7C000")
    return bytes(img[:PKG_FLASH])


def package(app, cfg_tool, ota):
    if hashlib.sha256(cfg_tool).hexdigest() != CFG_TOOL_SHA256:
        raise SystemExit("cfg_tool.bin is not the JieLi SDK file every Galaxia has")
    if LOADER_MARK not in ota[:0x100]:
        raise SystemExit("ota.bin: not a Galaxia SDK update loader")
    product = identity(app)
    return product, fm1pkg.ufw(flash_bin(app, cfg_tool), ota, product)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("loader"); l.add_argument("image", type=Path); l.add_argument("out", type=Path)
    p = sub.add_parser("package"); p.add_argument("app", type=Path); p.add_argument("out", type=Path)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--cfg-tool-from", type=Path, metavar="BACKUP", help="a backup from gx_flash.py backup")
    src.add_argument("--cfg-tool", type=Path, help="the JieLi SDK's cfg_tool.bin")
    p.add_argument("--ota", type=Path, default=HERE.parent / "build" / "loader" / "ota.bin")
    a = ap.parse_args()
    if a.cmd == "loader":
        ota = wrap_loader(a.image.read_bytes())
        a.out.write_bytes(ota)
        print("ota.bin: %d bytes (loader %d)" % (len(ota), a.image.stat().st_size))
        return
    cfg = (jl_package.cfg_tool_from(a.cfg_tool_from.read_bytes()) if a.cfg_tool_from else a.cfg_tool.read_bytes())
    product, pkg = package(a.app.read_bytes(), cfg, a.ota.read_bytes())
    a.out.write_bytes(pkg)
    print("%s: %d bytes, identity %s" % (a.out, len(pkg), product))


if __name__ == "__main__":
    main()
