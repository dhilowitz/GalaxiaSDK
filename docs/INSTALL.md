# Installing firmware on a Galaxia

> [!WARNING]
> **Use at your own risk.** Everything here is unofficial and experimental, provided as is, with no
> warranty of any kind (see sections 6 and 7 of the [licence](../LICENSE)). Replacing the pedal's
> firmware may void its warranty and can leave it not working; the recovery steps below have worked
> every time so far, but they are not guaranteed. A new firmware can make loud or unexpected sound:
> start with the volume down. You alone are responsible for what you do with your pedal. Not
> affiliated with or endorsed by M-VAVE or JieLi.

Everything goes over the pedal's USB-C port, through the chip's ROM bootloader. The tools run on
Linux (a Raspberry Pi works) and macOS. Building a firmware needs Linux x86-64; see the README.

## Once on macOS: libusb, Python, sudo

macOS's own disk driver takes the bootloader, and only root can take it back, so on a Mac
`gx_flash.py` runs with `sudo` (the soft key and the serial console do not). It talks to the
bootloader through libusb (`brew install libusb`). With sudo, a virtual environment keeps the Python
modules in reach:

```
python3 -m venv ~/.galaxia-venv
~/.galaxia-venv/bin/pip install pyusb pyyaml crcmod python-rtmidi
```

Then use `~/.galaxia-venv/bin/python` for `python3` below, with `sudo` in front of `gx_flash.py`
(for example `sudo ~/.galaxia-venv/bin/python tools/gx_flash.py backup my-galaxia.bin`). Files it
writes belong to you, not root. The serial console is `/dev/cu.usbmodem*`. If macOS says it cannot
read a disk while the pedal is in the bootloader, choose Ignore.

## Once on Linux: access and Python

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

Python needs `pyyaml` and `crcmod` (jl-uboot-tool's requirements): on Debian or Ubuntu
`sudo apt install python3-yaml python3-crcmod`, elsewhere `pip install pyyaml crcmod`.

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

## Updating over USB MIDI (experimental)

A firmware can go on over USB MIDI, with no bootloader and no sudo, on Linux, macOS or Windows, whether the pedal
runs Galaxia SDK firmware or M-VAVE's own. It uses the update protocol of M-VAVE's own updater, as felucca-bsp
implements it. Packages need two files from JieLi's AC79 SDK (Apache-2.0), which any Felucca release carries; get them
once, make a package from the app, then install it:

```
curl -LO https://hugelton.github.io/Felucca/firmware/felucca-1.8.1.fwsc      # once: any Felucca release
python3 tools/gx_pkg.py sdk-files felucca-1.8.1.fwsc                         # once: build/sdk/
python3 tools/gx_pkg.py package build/reverb/reverb.bin reverb.fwsc
pip install mido python-rtmidi                      # once
python3 tools/gx_update.py reverb.fwsc
```

Tested on a Galaxia running Galaxia SDK firmware, and on an Elemental Pro (M-VAVE's delay, the same board) running
M-VAVE's own firmware: the stock firmware checks the package, then hands over to the package's update loader.

The firmware checks the package and restarts into the update loader (`Galaxia Update`; LED red,
flickering green), which reads the whole package before erasing anything, writes the app area and
restarts into the new firmware. If it stops part way, run the same command again: the loader finishes the
write, as long as the pedal stayed powered: a power cut while the loader writes leaves a half-written firmware
(then use `gx_flash.py`; see "If a firmware does not start"). `gx_update.py --info` says what the pedal runs. The
upgrade command goes only to a pedal that answers as a Galaxia or an Elemental Pro. An update takes about half a minute
(`--slow` waits before each reply, if one ever stalls). macOS may keep showing the MIDI port under the firmware's
earlier name until the pedal is replugged.

A package holds the app area and the update loader (`build/loader/ota.bin`, built with every example), plus two
files from JieLi's AC79 SDK (Apache-2.0): `cfg_tool.bin`, in the app area, and `uboot.boot`, in the package's head,
which M-VAVE's firmware checks before it starts an update. No update loader writes the head: the pedal keeps its own.
A package holds nothing from M-VAVE. Ship `bsp/LICENSES/Apache-2.0.txt` with any package you give away.

**Back to M-VAVE's firmware without a backup**: M-VAVE publishes the stock firmware
([m-vave.com/download](https://www.m-vave.com/download): `Galaxia.fwsc`, and `ELEMENTAL PRO.fwsc` for the Elemental
Pro). `gx_update.py` installs it on a pedal running Galaxia SDK firmware; it knows the official files by their SHA-256
and takes no other. The SDK firmware can't tell which pedal it is on, so use the file for yours. A `gx_flash.py`
backup is still the fullest way back: it also keeps your pedal's own settings.

### From Chrome or Edge

`docs/installer/` does the same from a web page, using Web MIDI, with nothing to install, and the way back too
(choose M-VAVE's official file). Browsers only allow Web MIDI over https or localhost, so serve the page locally:

```
python3 -m http.server 8000 --bind 127.0.0.1 -d docs/installer      # then open http://localhost:8000/
```

Choose your `.fwsc` and press Install. The page takes Galaxia SDK packages and M-VAVE's official files, talks only to
MIDI ports named after the pedal (it has a field for other port names), and sends the upgrade command only to a pedal
that answers as a Galaxia or an Elemental Pro. If an install stops while the pedal is in update mode, press Install
again with the same file. `tools/gx_install_page.py` makes a copy of the page with firmwares built in, ready to put on
a website. On a Mac, the update
loader is a new USB device, so macOS may ask whether to allow it (System Settings > Privacy & Security > Allow
accessories to connect); if the loader doesn't appear, check that first.

For testing, `d` and Enter on the console makes the next update a dry run: the firmware checks the package
and stages the loader, then erases it and carries on (the updater then reports that no loader appeared).

## 3. Back to stock

```
python3 tools/gx_softkey.py
python3 tools/gx_flash.py restore my-galaxia.bin
```

Your backup is the fullest way back: **keep `my-galaxia.bin` somewhere safe, with a second copy.** Without one, M-VAVE's
official firmware goes back on over USB MIDI (see "Back to M-VAVE's firmware without a backup" above).

## If a firmware does not start

A Galaxia SDK firmware that fails to reach two seconds of running twice in a row restarts into the
bootloader by itself: run `gx_flash.py install` (or `restore`) again; no soft key needed. To start the
firmware already on the pedal without writing anything, `gx_flash.py run`. A firmware
that runs but misbehaves still answers the soft key and the console's `u`.

The bootloader is in the chip's ROM: writing the app area cannot remove it, and `gx_flash.py` never
writes the boot loader or its settings (flash 0x0000-0x3FFF) or the factory key area.
