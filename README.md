# Galaxia SDK (unofficial)

Write your own firmware for the **M-VAVE Galaxia** stereo reverb pedal. This is a board port and
runtime on top of [felucca-bsp](https://github.com/hugelton/felucca-bsp), plus the tools to back up,
install and restore firmware over USB. No soldering, no programmer.

> [!WARNING]
> **Use at your own risk.** This is unofficial, experimental software, provided as is, with no
> warranty of any kind (see sections 6 and 7 of the [licence](LICENSE)). Installing firmware that
> M-VAVE did not write may void your pedal's warranty. It can leave your pedal not working, and while
> every case met so far could be recovered over USB, that is not guaranteed. Firmware you build or
> install can also produce loud or unexpected sound: keep the volume down and protect your ears and
> speakers when trying something new. You alone are responsible for what you do with your pedal.
>
> Not affiliated with or endorsed by M-VAVE or JieLi. Product and company names belong to their
> owners. **Back up your pedal before anything else** ([docs/INSTALL.md](docs/INSTALL.md)).

## What you get

| | |
|---|---|
| `boards/galaxia/` | The Galaxia's wiring: audio link to its converter, the six knobs, footswitches, input jack detects, LED. Every pin checked on a real pedal. |
| `runtime/` | Everything a firmware needs besides the effect: boot, USB MIDI, USB audio both ways, a serial console, the watchdog, the fault report and a boot guard. An application defines five hooks. |
| `examples/passthrough/` | The smallest firmware: input to output, a mute footswitch. |
| `examples/reverb/` | A stereo reverb (8-line feedback delay network) with bypass, freeze, five knobs and room size on the selector. |
| `examples/diag/` | Audio link check: a 1 kHz tone on the left output, the raw input words and buffer counters on the console. |
| `tools/` | `gx_softkey.py` (into the bootloader), `gx_flash.py` (backup, install, restore, run), `jl_package.py` (the flash image). |
| `docs/` | [HARDWARE.md](docs/HARDWARE.md), [INSTALL.md](docs/INSTALL.md). |

## The hardware in short

JieLi WL82 / AC791N family (pi32v2 CPU with a single-precision FPU), 1 MiB of flash, about 512 KiB of
RAM, an external stereo converter at 44.1 kHz. With the runtime and USB, an effect has **about 410 KiB
of RAM** for its buffers (the reverb example uses 116 KiB of it) and runs every 128 frames.
The reverb example takes 13 % of the audio block. Details in [docs/HARDWARE.md](docs/HARDWARE.md).

> [!IMPORTANT]
> **Known problem:** an SDK firmware does not yet start when the pedal is powered up; the pedal comes up
> in its bootloader instead. `python3 tools/gx_flash.py run` starts it (see
> [docs/INSTALL.md](docs/INSTALL.md)). Restarts from the firmware itself work.

## Build

Needs Linux x86-64 (or Docker), Python 3 and JieLi's toolchain (clang 4.0.1 for pi32v2):

```
git clone --recurse-submodules <this repository>
JIELI_TOOLCHAIN=~/.jieli/toolchain ./build.sh reverb     # -> build/reverb/reverb.bin
```

felucca-bsp's [Felucca](https://github.com/hugelton/Felucca) has `tools/get_toolchain.sh`, which
fetches the toolchain from JieLi's package server.

## Install

See [docs/INSTALL.md](docs/INSTALL.md). In short, on Linux with the pedal on USB:

```
python3 tools/gx_softkey.py                         # into the bootloader
python3 tools/gx_flash.py backup my-galaxia.bin     # FIRST: your pedal's own firmware, kept safe
python3 tools/gx_softkey.py
python3 tools/gx_flash.py install build/reverb/reverb.bin my-galaxia.bin
```

`gx_flash.py restore my-galaxia.bin` puts the stock firmware back. The SDK ships no M-VAVE or JieLi
files: the boot loader and its settings stay on your pedal, and the one other file an image needs
comes from your own backup.

## Writing a firmware

One C file that includes `gx_runtime.c` and defines the hooks; any `fx_*.c` files beside it are
compiled for speed:

```c
#define GX_USB_PRODUCT "My Effect"
#include "gx_runtime.c"

static void app_init(void) { }
static void app_audio(const int32_t *in, int32_t *out, uint32_t frames) { /* 128 stereo frames */ }
static void app_poll(uint32_t now_ms) { /* gx_in.knob[], gx_in.switches, gx_led(), gx_selector_position() */ }
static int app_command(const char *line, uint32_t n) { return 0; }
static void app_status(void) { }
```

`runtime/gx_runtime.c` documents the hooks and services. Keep floating-point code in the audio
interrupt (`app_audio` and what it calls): the interrupt entry does not save FPU state.

## Before you distribute a firmware

The runtime uses USB VID 0x1209 / PID 0x0007, a [pid.codes](https://pid.codes) **test** ID: fine on
your bench, not for release. Get your own PID (free for open-source projects) and set `GX_USB_PID`.

## Licence and credits

MPL-2.0 ([LICENSE](LICENSE)); see [NOTICE](NOTICE) for what this builds on and who to thank.
