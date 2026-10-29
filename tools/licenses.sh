#!/bin/sh
# Gather the licenses of what is linked into a release's Linux programs,
# for its LICENSES directory: this program's, the dependencies' from a
# source tarball's tree, and the C and C++ runtimes' from this system's
# packages, so run it where the programs were built (or alike).
#
#   tools/licenses.sh SRC OUT [gui]
#
# SRC is an unpacked source tarball (tools/dist.py), with deps/. With
# gui, also the GUI's: SDL3, FreeType, Dear ImGui, md4c, and the fonts.
# A missing file is an error.
set -eu
src=$1
out=$2
gui=${3:-}

mkdir -p "$out"
cp "$src/UNLICENSE" "$out/prospero.txt"
cp "$src/deps/mbedtls/LICENSE" "$out/mbedtls.txt"
cp "$src/deps/cpp-httplib/LICENSE" "$out/cpp-httplib.txt"
# Wuffs, a single file, carries its notice in a comment (Apache-2.0 OR MIT)
for f in "$src"/deps/wuffs/wuffs-*.c; do break; done
sed -n '/^\/\/ Copyright/,/^\/\/ SPDX-License-Identifier/s|^// \{0,1\}||p' "$f" \
    >"$out/wuffs.txt"
test -s "$out/wuffs.txt"
# glibc, and GCC's runtime (its library exception), as Debian and
# Ubuntu record them
cp /usr/share/doc/libc6/copyright "$out/glibc.txt"
cp /usr/share/doc/libgcc-s1/copyright "$out/gcc-runtime.txt"

if [ "$gui" = gui ]; then
    cp "$src/deps/sdl3/LICENSE.txt" "$out/sdl3.txt"
    cat "$src/deps/freetype/LICENSE.TXT" "$src/deps/freetype/docs/FTL.TXT" \
        >"$out/freetype.txt"
    cp "$src/deps/imgui/LICENSE.txt" "$out/imgui.txt"
    cp "$src/deps/md4c/LICENSE.md" "$out/md4c.txt"
    cp "$src/assets/FONTS" "$out/fonts.txt"
fi
