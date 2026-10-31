# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
# The layout and records follow fm1_open's sdk/Package.cs (MIT, Copyright (c) 2023 Andrey Grigoryev
# (kagaimiq), 2026 M-VAVE), itself after kagaimiq's jl-misctools.
"""JieLi AC791N app-area packaging, plus a checker for existing flash dumps.

The Galaxia keeps its own boot head (flash 0x0000-0x3FFF: header, uboot.boot,
isd_config.ini). Only the app area from 0x4000 is generated here.

usage:
  jl_package.py check <flash.bin>                      validate a dump's app area
  jl_package.py image <app.bin> <backup.bin> <out.bin> a flash image: the app, with the backup's boot head
                                                       and its cfg_tool.bin
"""
import struct, sys

AREA, ENTRY, APP_DATA = 0x4000, 0x02000120, 0x120
FLASH_LIMIT, USER_SIZE, BT_SIZE, MIN_VM = 0xFF000, 536576, 4096, 32768
CHIP_KEY = 0x980F


def crc16(data, value=0):
    for b in data:
        value ^= b << 8
        for _ in range(8):
            value = ((value << 1) ^ 0x1021 if value & 0x8000 else value << 1) & 0xFFFF
    return value


def cipher(buf, begin, length, state):
    for i in range(begin, begin + length):
        buf[i] ^= state & 0xFF
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xFFFF


def sfc(buf, start, length, key=CHIP_KEY):
    """XIP cipher over [start, start+length) of the app area (offsets from AREA)."""
    for p in range(0, length, 32):
        cipher(buf, AREA + start + p, min(32, length - p), key ^ ((start + p) >> 2))


def record(img, off, name, crc, address, size, attributes, last, extra=None):
    img[off:off + 32] = bytes(32)
    struct.pack_into("<HIIHH", img, off + 2, crc, address, size, attributes, last)
    img[off + 16:off + 32] = b"\xff" * 16
    n = name.encode()
    img[off + 16:off + 16 + len(n)] = n
    img[off + 16 + len(n)] = 0
    if extra is not None:
        struct.pack_into("<I", img, off + 28, extra)
    struct.pack_into("<H", img, off, crc16(img[off + 2:off + 32]))


def layout(app_len):
    image_size = (AREA + APP_DATA + app_len + 383 + 64 + 4095) & ~4095
    user_at = FLASH_LIMIT - USER_SIZE
    vm_end = user_at - BT_SIZE
    if image_size > vm_end - MIN_VM:
        raise ValueError("application does not fit")
    return image_size, vm_end


def build(app, tool, head, size=0x100000):
    """Flash image: head[0:AREA] unchanged, new app area, rest erased."""
    image_size, vm_end = layout(len(app))
    img = bytearray(b"\xff" * size)
    img[:AREA] = head[:AREA]
    payload = APP_DATA + len(app) + len(tool)
    a = AREA + APP_DATA
    img[a:a + len(app)] = app
    img[a + len(app):a + len(app) + len(tool)] = tool
    record(img, AREA + 32, "app.bin", crc16(app), APP_DATA, len(app), 0xFF82, 0)
    record(img, AREA + 64, "cfg_tool.bin", crc16(tool), APP_DATA + len(app), len(tool), 0xFF82, 0)
    record(img, AREA + 96, "VM", 0xFFFF, image_size, vm_end - image_size, 0x8112, 0, MIN_VM)
    record(img, AREA + 128, "PRCT", 0xFFFF, 0, image_size, 0x8292, 0, 0x45444F43)
    record(img, AREA + 160, "BTIF", 0xFFFF, vm_end, BT_SIZE, 0x8192, 1, 0x4F545541)
    record(img, AREA, "app_area_head", crc16(img[AREA + 32:AREA + payload]), ENTRY, payload, 0xFF83, 0)
    reserved = AREA + payload
    record(img, reserved + 32, "USER", 0xFFFF, FLASH_LIMIT - USER_SIZE, USER_SIZE, 0x8192, 1, 0x4F545541)
    record(img, reserved, "EXT_RESERVED", crc16(img[reserved + 32:reserved + 64]), 32, 64, 0xFF93, 1)
    sfc(img, 0, payload + 64)
    return img, image_size


def parse(flash, key=CHIP_KEY):
    """Decrypt the app area directory and report each record with CRC checks."""
    buf = bytearray(flash)
    head = bytearray(buf[AREA:AREA + 32])
    cipher(head, 0, 32, key)
    _, _, payload, _, _ = struct.unpack_from("<HIIHH", head, 2)
    sfc(buf, 0, payload + 64, key)
    out = []
    offs = list(range(AREA, AREA + 0x200, 32)) + [AREA + payload, AREA + payload + 32]
    for off in offs:
        r = buf[off:off + 32]
        if r[:4] == b"\xff\xff\xff\xff":
            continue
        hcrc, dcrc, addr, size, attr, last = struct.unpack_from("<HHIIHH", r, 0)
        name = r[16:28].split(b"\0")[0].decode("ascii", "replace")
        ok_head = hcrc == crc16(r[2:32])
        ok_data = None
        if name in ("app.bin", "cfg_tool.bin"):
            ok_data = dcrc == crc16(buf[AREA + addr:AREA + addr + size])
        elif name == "app_area_head":
            ok_data = dcrc == crc16(buf[AREA + 32:AREA + size])
        elif name == "EXT_RESERVED":
            ok_data = dcrc == crc16(buf[off + 32:off + 64])
        out.append((off, name, addr, size, attr, last, ok_head, ok_data))
    return out, buf


def cfg_tool_from(flash):
    """The cfg_tool.bin record of a flash dump (decrypted)."""
    recs, dec = parse(flash)
    for off, name, addr, size, attr, last, oh, od in recs:
        if name == "cfg_tool.bin" and oh and od:
            return bytes(dec[AREA + addr:AREA + addr + size])
    raise ValueError("no valid cfg_tool.bin in the backup")


def image_from_backup(app, backup):
    img, _ = build(app, cfg_tool_from(backup), backup)
    return bytes(img)


def main():
    if sys.argv[1] == "check":
        recs, _ = parse(open(sys.argv[2], "rb").read())
        for off, name, addr, size, attr, last, oh, od in recs:
            print("0x%05X %-14s addr=0x%08X size=0x%06X attr=%04X last=%d head_crc=%s data_crc=%s"
                  % (off, name, addr, size, attr, last, oh, od))
    elif sys.argv[1] == "image":
        app = open(sys.argv[2], "rb").read()
        backup = open(sys.argv[3], "rb").read()
        img, image_size = build(app, cfg_tool_from(backup), backup)
        open(sys.argv[4], "wb").write(img)
        print("app area 0x%X..0x%X (%d bytes app)" % (AREA, image_size, len(app)))


if __name__ == "__main__":
    main()
