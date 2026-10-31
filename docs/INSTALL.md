# Installing firmware on a Galaxia

> [!WARNING]
> **Use at your own risk.** Everything here is unofficial and experimental, provided as is, with no
> warranty of any kind (see sections 6 and 7 of the [licence](../LICENSE)). Replacing the pedal's
> firmware may void its warranty and can leave it not working; the recovery steps below have worked
> every time so far, but they are not guaranteed. A new firmware can make loud or unexpected sound:
> start with the volume down. You alone are responsible for what you do with your pedal. Not
> affiliated with or endorsed by M-VAVE or JieLi.

Everything goes over the pedal's USB-C port, through the chip's ROM bootloader. The tools run on
Linux (jl-uboot-tool talks to the bootloader as a SCSI device); a Raspberry Pi works.

## Once: access and Python

The bootloader shows up as a USB disk, `WL82 UBOOT1.00` (USB 4c4a:8057). Let your user open it and
the pedal's MIDI port without root, e.g. as `/etc/udev/rules.d/70-galaxia.rules`:

```
# Galaxia: stock firmware and its bootloader (JieLi 4c4a), Galaxia SDK firmware (1209)
SUBSYSTEM=="usb", ATTR{idVendor}=="4c4a", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="sound", ATTRS{idVendor}=="4c4a", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="scsi_generic", ATTRS{idVendor}=="4c4a", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="1209", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="sound", ATTRS{idVendor}=="1209", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="tty", ATTRS{idVendor}=="1209", MODE="0660", GROUP="plugdev"
```

then `sudo udevadm control --reload-rules && sudo udevadm trigger`. The `sg` kernel module must be
loaded (`sudo modprobe sg` if `/sys/class/scsi_generic` is missing).

Python needs `pyyaml` and `crcmod` (jl-uboot-tool's requirements).

## 1. Back up your pedal (do not skip)

```
python3 tools/gx_softkey.py                         # the stock firmware restarts into the bootloader
python3 tools/gx_flash.py backup my-galaxia.bin
```

It reads the whole 1 MiB flash twice and compares the reads. Keep this file: it is your pedal's own
stock firmware, the way back, and the source of the boot loader settings every install reuses.
Never share it or commit it: it is M-VAVE's firmware.

`gx_flash.py` restarts the pedal when it is done (the bootloader leaves it running its firmware).

## 2. Install

```
python3 tools/gx_softkey.py
python3 tools/gx_flash.py install build/reverb/reverb.bin my-galaxia.bin
```

It checks the chip and flash, checks that the pedal's boot loader matches your backup's, writes the
app area, reads it back, and restarts. The pedal comes back as a USB MIDI + audio + serial device
(`Galaxia Reverb` for the example). The serial console (`/dev/ttyACM0`, any speed) prints a status
line twice a second; `h` lists its commands.

## 3. Back to stock

```
python3 tools/gx_softkey.py
python3 tools/gx_flash.py restore my-galaxia.bin
```

## If a firmware does not start

A Galaxia SDK firmware that fails to reach two seconds of running twice in a row restarts into the
bootloader by itself: run `gx_flash.py install` (or `restore`) again; no soft key needed. A firmware
that runs but misbehaves still answers the soft key and the console's `u`.

The bootloader is in the chip's ROM: writing the app area cannot remove it, and `gx_flash.py` never
writes the boot loader or its settings (flash 0x0000-0x3FFF) or the factory key area.
