#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build an example: build/<name>/<name>.bin (app.bin: offset 0 runs at 0x02000120), and the update loader,
# build/loader/ota.bin (tools/gx_pkg.py package makes an update package from the two).
#   JIELI_TOOLCHAIN=~/.jieli/toolchain ./build.sh reverb
# Every .c file in examples/<name>/ is compiled; main.c includes runtime/gx_runtime.c.
# Files named fx_*.c (the DSP) are compiled for speed (-O2), the rest for size (-Os).
# After felucca-bsp's examples/hwtest/build.sh (Leo Kuroshita, Hügelton Instruments).
set -e
NAME="${1:?usage: build.sh <example>   (examples/: $(ls "$(dirname "$0")/examples" | tr '\n' ' '))}"
TC="${JIELI_TOOLCHAIN:?JIELI_TOOLCHAIN must point at the JieLi Linux toolchain}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
BSP="$ROOT/bsp"
SRC="$ROOT/examples/$NAME"
O="$ROOT/build/$NAME"
[ -f "$BSP/chip/crt0.S" ] || { echo "build.sh: bsp/ is empty: git submodule update --init"; exit 1; }
[ -f "$SRC/main.c" ] || { echo "build.sh: no examples/$NAME/main.c"; exit 1; }
rm -rf "$O"; mkdir -p "$O"
CC="$TC/pi32v2/bin/clang -target pi32v2"
INC="-I$BSP/chip -I$ROOT/boards/galaxia -I$ROOT/runtime -I$SRC -I$BSP/boards/fm1"   # (boards/fm1: fm1_adc.h only)
# -mfprev1: the FPU. Float code belongs in the audio interrupt only (the runtime's uac_play is float too):
# the interrupt entry does not save FPU state.
# -pi32v2-large-program: calls reach between flash (XIP, 0x02000000) and RAM code (0x01C00000), as
# M-VAVE's FM1 kit builds; code placed in .ram_text (fast, safe during flash writes) needs it.
CFLAGS="-mcpu=r3 -mfprev1 -mllvm -pi32v2-large-program=true -ffunction-sections -fno-builtin -Wall -Wno-unused-function"
$CC -c "$BSP/chip/crt0.S" -o "$O/crt0.o"
$CC -c "$BSP/chip/fm1_vec.S" -o "$O/vec.o"
$CC -c "$BSP/chip/fm1_isr.S" -o "$O/isr.o"
OBJS=""
for f in "$SRC"/*.c; do
    o="$O/$(basename "$f" .c).o"
    case "$(basename "$f")" in fx_*) OPT=-O2 ;; *) OPT=-Os ;; esac
    $CC $CFLAGS $OPT $INC -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
"$TC/pi32v2/bin/ld" -T "$ROOT/runtime/app.ld" "$O/crt0.o" "$O/vec.o" "$O/isr.o" $OBJS -o "$O/$NAME.elf"
for s in text data ram_text; do
    "$TC/common/bin/objcopy" -O binary -j ".$s" "$O/$NAME.elf" "$O/$s.bin"
done
python3 - "$TC" "$O" "$NAME" <<'PY'
import re, subprocess, sys
tc, out, name = sys.argv[1:]
syms = subprocess.run([f"{tc}/common/bin/objdump", "-t", f"{out}/{name}.elf"], capture_output=True, text=True, check=True).stdout
sym = lambda n: int(re.search(r"^([0-9a-f]+) .*\s" + n + r"$", syms, re.M).group(1), 16)
assert sym("_start") == 0x02000120, hex(sym("_start"))
img = bytearray(open(f"{out}/text.bin", "rb").read())
parts = [(sym("_rt_load"), open(f"{out}/ram_text.bin", "rb").read()), (sym("_data_load"), open(f"{out}/data.bin", "rb").read())]
for at, part in sorted(parts):
    if part:
        assert at - 0x02000120 >= len(img), "sections overlap"
        img += b"\xff" * (at - 0x02000120 - len(img)) + part
img += b"\xff" * (-len(img) % 4)
open(f"{out}/{name}.bin", "wb").write(img)
ram = sym("_bss_end") - 0x01C08000
print(f"{name}.bin: {len(img)} bytes; RAM {ram} of {0x6C000} bytes used ({(0x6C000 - ram) // 1024} KiB free), "
      f"RAM code {sym('_rt_end') - sym('_rt_start')} of 24576")
PY

# The update loader (loader/gx_loader.c), the same for every firmware: build/loader/ota.bin, which
# tools/gx_pkg.py puts in update packages. A RAM image: nothing in it may run from flash.
L="$ROOT/build/loader"
mkdir -p "$L"
$CC -c "$BSP/loader/crt0_ldr.S" -o "$L/crt0_ldr.o"
$CC $CFLAGS -Os -I$BSP/chip -I$BSP/loader -c "$ROOT/loader/gx_loader.c" -o "$L/gx_loader.o"
"$TC/pi32v2/bin/ld" --gc-sections -e _start -T "$BSP/loader/loader.ld" "$L/crt0_ldr.o" "$L/gx_loader.o" -o "$L/loader.elf"
"$TC/common/bin/objcopy" -O binary -j .text "$L/loader.elf" "$L/loader.bin"
"$TC/common/bin/objdump" -h "$L/loader.elf" | python3 -c '
import sys
bad = [p[1] for p in (l.split() for l in sys.stdin) if len(p) > 3 and p[1].startswith(".") and int(p[2], 16) and int(p[3], 16) >= 0x02000000]
sys.exit("build.sh: loader sections outside RAM: %s" % bad if bad else 0)'
python3 "$ROOT/tools/gx_pkg.py" loader "$L/loader.bin" "$L/ota.bin"
