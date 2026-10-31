#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 David Hilowitz
# Fetch JieLi's Linux toolchain (clang 4.0.1 for pi32v2; Linux x86-64) and point DEST/toolchain at it.
#   tools/get_toolchain.sh [DEST [ARCHIVE]]   DEST defaults to ~/.jieli
# Then build with JIELI_TOOLCHAIN=DEST/toolchain ./build.sh <example>.
# The archive comes from JieLi's package server (it redirects to a mirror in China, which can be slow or
# refuse connections), or give ARCHIVE: a jieli-linux-toolchains-*.tar.xz fetched some other way. The SDK
# was tested with the release below; a different one is unpacked too, with a warning (check that the
# examples still build and run).
set -e
DEST="${1:-$HOME/.jieli}"
ARCHIVE="$2"
URL="https://pkgman.jieliapp.com/s/linux-toolchain"
TESTED_NAME="jieli-linux-toolchains-20260730.1.tar.xz"
TESTED_SHA256="a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd"

mkdir -p "$DEST"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
if [ -n "$ARCHIVE" ]; then
    cp "$ARCHIVE" "$TMP/toolchain.tar.xz"
else
    echo "downloading $URL"
    curl -fL --retry 3 -o "$TMP/toolchain.tar.xz" "$URL" ||
        { echo "download failed: try again later, or fetch the archive another way and pass it as ARCHIVE"; exit 1; }
fi
SUM="$(sha256sum "$TMP/toolchain.tar.xz" | cut -d' ' -f1)"
if [ "$SUM" = "$TESTED_SHA256" ]; then
    echo "checksum matches the tested release ($TESTED_NAME)"
else
    echo "warning: not the release this SDK was tested with ($TESTED_NAME); sha256 $SUM"
fi
tar -xJf "$TMP/toolchain.tar.xz" -C "$DEST"
TC="$(ls -d "$DEST"/jieli-linux-toolchains-* | sort | tail -1)"
ln -sfn "$(basename "$TC")" "$DEST/toolchain"
"$DEST/toolchain/pi32v2/bin/clang" --version | head -1
echo "export JIELI_TOOLCHAIN=$DEST/toolchain"
