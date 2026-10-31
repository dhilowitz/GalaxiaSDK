# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
"""SCSI commands to the pedal's bootloader on macOS, for jl-uboot-tool.

The bootloader is a USB mass storage device. jl-uboot-tool talks to it through the operating system's SCSI
pass-through (Linux SG_IO, Windows), which macOS does not offer for such a device, so this sends the commands
over USB itself (the mass storage bulk-only transport) with libusb, through pyusb. macOS's own storage driver
holds the device, and only root may take it over: run gx_flash.py with sudo.

Importing this module puts its SCSIDev and SCSIException in place of jl-uboot-tool's scsiio package (which
has no macOS port) before jltech is imported.
"""
import os, struct, sys, types
from ctypes.util import find_library

import usb.backend.libusb1
import usb.core
import usb.util

TIMEOUT_MS = 10000
CBW_SIG, CSW_SIG = 0x43425355, 0x53425355
LIBUSB_ERROR_PIPE = -9                          # a stalled endpoint
UBOOT = ("WL82", "UBOOT1.00")                   # what the bootloader answers to INQUIRY


class SCSIException(Exception):
    pass


def backend():
    # sudo clears the library search path, so look where Homebrew and MacPorts put libusb as well
    for path in (find_library("usb-1.0"), "/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib",
                 "/opt/local/lib/libusb-1.0.dylib"):
        if path and (not path.startswith("/") or os.path.exists(path)):
            b = usb.backend.libusb1.get_backend(find_library=lambda _name, p=path: p)
            if b:
                return b
    raise SystemExit("libusb not found: install it (Homebrew: brew install libusb)")


class SCSIDev:
    """One USB mass storage interface, claimed from macOS's driver; jl-uboot-tool's execute()."""

    def __init__(self, dev=None):
        self.is_open = False
        self.tag = 0
        if dev is not None:
            self.open(dev)

    def open(self, dev):
        self.dev = dev
        intf = dev.get_active_configuration()[(0, 0)]
        self.intf = intf.bInterfaceNumber
        self.ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: not e.bEndpointAddress & 0x80)
        self.ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: e.bEndpointAddress & 0x80)
        try:
            if dev.is_kernel_driver_active(self.intf):
                dev.detach_kernel_driver(self.intf)
            usb.util.claim_interface(dev, self.intf)
        except usb.core.USBError as e:
            if e.errno == 13:
                raise SystemExit("macOS would not hand over the pedal's bootloader: run this with sudo")
            raise
        self.is_open = True

    def close(self):
        if self.is_open:
            try:
                usb.util.release_interface(self.dev, self.intf)
            except usb.core.USBError:
                pass                            # gone already (the pedal restarted)
            usb.util.dispose_resources(self.dev)
            self.is_open = False

    def _clear(self, ep, e):
        if e.backend_error_code != LIBUSB_ERROR_PIPE:
            raise SCSIException("USB transfer failed: %s" % e)
        self.dev.clear_halt(ep)

    def _status(self):
        for _ in range(2):                      # a stall before the status: clear it and read again
            try:
                return bytes(self.ep_in.read(13, TIMEOUT_MS))
            except usb.core.USBError as e:
                self._clear(self.ep_in, e)
        raise SCSIException("no command status from the device")

    def execute(self, cdb, data_out, data_in, max_sense_len=32, return_sense_buffer=False):
        if data_out and data_in:
            raise SCSIException("a command can move data one way only")
        self.tag = (self.tag + 1) & 0xFFFFFFFF
        n = len(data_out) if data_out else len(data_in) if data_in else 0
        cbw = struct.pack("<IIIBBB", CBW_SIG, self.tag, n, 0x80 if data_in else 0, 0, len(cdb))
        self.ep_out.write(cbw + bytes(cdb).ljust(16, b"\0"), TIMEOUT_MS)
        try:
            if data_out:
                self.ep_out.write(bytes(data_out), TIMEOUT_MS)
            elif data_in:
                got = bytes(self.ep_in.read(n, TIMEOUT_MS))
                data_in[:len(got)] = got
        except usb.core.USBError as e:
            self._clear(self.ep_out if data_out else self.ep_in, e)
        sig, tag, resid, status = struct.unpack("<IIIB", self._status())
        if sig != CSW_SIG or tag != self.tag:
            raise SCSIException("bad command status from the device")
        if status != 0:
            raise SCSIException("command 0x%02X failed (status %d)" % (cdb[0], status))
        return (resid, b"") if return_sense_buffer else resid


def inquiry(s):
    data = bytearray(36)
    s.execute(bytes([0x12, 0, 0, 0, 36, 0]), None, data)
    return data[8:16].decode(errors="replace").strip(), data[16:32].decode(errors="replace").strip()


def open_uboot():
    """The one pedal in the bootloader, opened."""
    found = []
    for dev in usb.core.find(find_all=True, backend=backend()):
        try:
            intf = dev.get_active_configuration()[(0, 0)]
        except usb.core.USBError:
            continue
        if intf.bInterfaceClass != 8 or intf.bInterfaceProtocol != 0x50:   # mass storage, bulk-only
            continue
        if dev.idVendor != 0x4C4A:              # JieLi's bootloader: leave other disks alone
            continue
        s = SCSIDev(dev)
        try:
            vendor, model = inquiry(s)
        except SCSIException:
            s.close()
            continue
        if vendor.upper() == UBOOT[0] and model.upper().startswith(UBOOT[1]):
            found.append(s)
        else:
            s.close()
    if len(found) != 1:
        for s in found:
            s.close()
        raise SystemExit("need exactly one pedal in the bootloader (WL82 UBOOT1.00), found %d" % len(found))
    return found[0]


_pkg = types.ModuleType("scsiio")
_pkg.__path__ = []
_pkg.SCSIDev = SCSIDev
_common = types.ModuleType("scsiio.common")
_common.SCSIException = SCSIException
_common.SCSIDevBase = object
_pkg.common = _common
sys.modules["scsiio"] = _pkg
sys.modules["scsiio.common"] = _common
