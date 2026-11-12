#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""Update packages for the Galaxia: what gx_update.py installs over USB MIDI.

  gx_pkg.py loader LOADER.bin OTA.bin
      wrap the update loader (build/loader/loader.bin, a RAM image for 0x01C0A800) as the boot loader loads
      it: a file head, a load head, then blocks of at most 4 KiB, each LZ4-compressed (build.sh does this)
  gx_pkg.py sdk-files PACKAGE.fwsc [DIR]
      take JieLi's AC79 SDK files that packages need, uboot.boot and cfg_tool.bin (Apache-2.0), out of a package
      that carries them, such as any Felucca release (hugelton.github.io/Felucca/firmware/), into DIR (build/sdk).
      Both are checked against the SDK's own (AC79NN_SDK_V1.2.1).
  gx_pkg.py package APP.bin OUT.fwsc [--sdk DIR] [--cfg-tool-from BACKUP.bin] [--ota OTA.bin]
      a package (M-UPGRADE .fwsc, the format of M-VAVE's own updates): the app area as gx_flash.py install
      writes it, with build/loader/ota.bin as its update loader. Its identity ("SDK-Galaxia_900") is read from the
      app (GX_UPDATE_ID). The SDK files come from DIR (build/sdk); --cfg-tool-from takes cfg_tool.bin from a
      backup instead (it is the same file on every Galaxia seen).

The package's head (flash 0x0000-0x3FFF) is built from the SDK's uboot.boot, as felucca-bsp's fm1pkg.py builds
Felucca's: M-VAVE's firmware checks it before it starts an update (a blank head is refused), so with it a pedal still
on M-VAVE's firmware can install a package over USB MIDI. Nothing writes it to the pedal: the update loaders write
the app area only and the pedal keeps its own head. The UFW container and the identity marker come from fm1pkg.py
(MPL-2.0).
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
UBOOT_SHA256 = "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4"   # AC79NN_SDK_V1.2.1 uboot.boot
SDK_DIR = HERE.parent / "build" / "sdk"
ISD_NAME = b"[GALAXIA-SDK]\r\n"
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


def boot_head(uboot, isd_name=ISD_NAME, key=fm1pkg.KEY):
    """flash 0x0000-0x3FFF as fm1pkg.flash_image() builds it: the flash header, a JLFS directory (uboot.boot,
    isd_config.ini, app_dir_head, key_mac), the SDK's SPL and a minimal isd_config holding the chip key"""
    isd = fm1pkg.key_blob(key) + isd_name
    f = bytearray(b"\xff" * jl_package.AREA)
    spl_off = 0xA0
    isd_off = spl_off + len(uboot)
    f[spl_off:isd_off] = uboot
    f[isd_off:isd_off + len(isd)] = isd
    top = (fm1pkg.entry(fm1pkg.crc16(uboot), spl_off, len(uboot), 0x00, 0x00, 0, "uboot.boot") +
           fm1pkg.entry(fm1pkg.crc16(isd), isd_off, len(isd), 0x02, 0x80, 0, "isd_config.ini") +
           fm1pkg.entry(0xFFFF, 0x4000, 0xFFFFFFFF, 0x81, 0xFF, 0, "app_dir_head") +
           fm1pkg.entry(0xFFFF, 0xFF000, 0x1000, 0x12, 0x01, 0xFFFF, "key_mac"))
    h = bytearray(struct.pack("<H", fm1pkg.crc16(fm1pkg.FLASH_HDR)) + fm1pkg.FLASH_HDR + top)
    for o in range(0, len(h), 32):
        fm1pkg.enc(h, o, 32)
    f[:len(h)] = h
    return bytes(f)


def sdk_files(pkg):
    """uboot.boot and cfg_tool.bin out of a package built with them (a Felucca release) -> (uboot, cfg_tool)"""
    raw = Path(pkg).read_bytes()
    logical = b"".join(raw[i * 0x30:i * 0x30 + 0x2F] for i in range(20)) + raw[20 * 0x30:]
    hdr = bytearray(logical[:0x40])
    fm1pkg.enc(hdr, 0, 0x40)
    count = struct.unpack_from("<H", hdr, 8)[0]
    flash = None
    for i in range(count):
        e = bytearray(logical[0x40 + i * 0x50:0x40 + (i + 1) * 0x50])
        fm1pkg.enc(e, 0, 0x50)
        off, size = struct.unpack_from("<II", e, 8)
        if e[0x40:0x50].split(b"\0")[0] == b"flash.bin":
            flash = logical[off:off + size]
    if not flash:
        raise SystemExit("%s: no flash.bin in it" % pkg)
    uboot = None
    for o in range(0x20, 0xA0, 32):
        e = bytearray(flash[o:o + 32])
        fm1pkg.enc(e, 0, 32)
        if e[16:].split(b"\0")[0] == b"uboot.boot":
            off, size = struct.unpack_from("<II", e, 4)
            uboot = bytes(flash[off:off + size])
    cfg = jl_package.cfg_tool_from(bytes(flash).ljust(0x100000, b"\xff"))
    if not uboot or hashlib.sha256(uboot).hexdigest() != UBOOT_SHA256:
        raise SystemExit("%s: its uboot.boot is not the SDK's" % pkg)
    if hashlib.sha256(cfg).hexdigest() != CFG_TOOL_SHA256:
        raise SystemExit("%s: its cfg_tool.bin is not the SDK's" % pkg)
    return uboot, cfg


def flash_bin(app, cfg_tool, uboot):
    img, _ = jl_package.build(app, cfg_tool, boot_head(uboot))
    if any(b != 0xFF for b in img[0x7C000:]):
        raise SystemExit("the app area runs past 0x7C000")
    return bytes(img[:PKG_FLASH])


def package(app, cfg_tool, ota, uboot):
    if hashlib.sha256(cfg_tool).hexdigest() != CFG_TOOL_SHA256:
        raise SystemExit("cfg_tool.bin is not the JieLi SDK file every Galaxia has")
    if hashlib.sha256(uboot).hexdigest() != UBOOT_SHA256:
        raise SystemExit("uboot.boot is not the AC79 SDK's (tools/gx_pkg.py sdk-files gets it)")
    if LOADER_MARK not in ota[:0x100]:
        raise SystemExit("ota.bin: not a Galaxia SDK update loader")
    product = identity(app)
    return product, fm1pkg.ufw(flash_bin(app, cfg_tool, uboot), ota, product)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("loader"); l.add_argument("image", type=Path); l.add_argument("out", type=Path)
    s = sub.add_parser("sdk-files"); s.add_argument("package", type=Path)
    s.add_argument("dir", type=Path, nargs="?", default=SDK_DIR)
    p = sub.add_parser("package"); p.add_argument("app", type=Path); p.add_argument("out", type=Path)
    p.add_argument("--sdk", type=Path, default=SDK_DIR, metavar="DIR", help="uboot.boot and cfg_tool.bin (sdk-files)")
    p.add_argument("--cfg-tool-from", type=Path, metavar="BACKUP", help="cfg_tool.bin from a gx_flash.py backup")
    p.add_argument("--ota", type=Path, default=HERE.parent / "build" / "loader" / "ota.bin")
    a = ap.parse_args()
    if a.cmd == "sdk-files":
        uboot, cfg = sdk_files(a.package)
        a.dir.mkdir(parents=True, exist_ok=True)
        (a.dir / "uboot.boot").write_bytes(uboot)
        (a.dir / "cfg_tool.bin").write_bytes(cfg)
        print("%s: uboot.boot and cfg_tool.bin (the AC79 SDK's)" % a.dir)
        return
    if a.cmd == "loader":
        ota = wrap_loader(a.image.read_bytes())
        a.out.write_bytes(ota)
        print("ota.bin: %d bytes (loader %d)" % (len(ota), a.image.stat().st_size))
        return
    try:
        uboot = (a.sdk / "uboot.boot").read_bytes()
        cfg = (jl_package.cfg_tool_from(a.cfg_tool_from.read_bytes()) if a.cfg_tool_from
               else (a.sdk / "cfg_tool.bin").read_bytes())
    except OSError as e:
        raise SystemExit("%s: %s. Get the SDK files once with: tools/gx_pkg.py sdk-files FELUCCA.fwsc" % (e.filename, e.strerror))
    product, pkg = package(a.app.read_bytes(), cfg, a.ota.read_bytes(), uboot)
    a.out.write_bytes(pkg)
    print("%s: %d bytes, identity %s" % (a.out, len(pkg), product))


if __name__ == "__main__":
    main()
