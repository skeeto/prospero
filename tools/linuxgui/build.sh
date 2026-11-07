#!/bin/sh
# Build, test, check and pack the Linux GUI: the release's linux-gui job
# (the Community Edition), where install.sh set up the tools, and
# tools/itch.py (-DPROSPERO_EDITION=itch), in the toolchain image
# (tools/toolchain) with its glibc 2.28 cross compilers.
#
#   sh tools/linuxgui/build.sh SRC BUILD OUT [CMAKE_OPTION ...]
#
# SRC is an unpacked source tarball (tools/dist.py), BUILD a build
# directory, and OUT gets prospero-VERSION-linux-ARCH.tar.gz: the
# program, its desktop file and icon, a README.txt for its edition,
# and LICENSES. The program must need only glibc's own libraries, and
# none of their symbols past the floor, and must have SDL's Wayland and
# X11 video and its D-Bus, which load their libraries by name.
#
# Its tests and the program itself run directly, so a cross build's
# (CMAKE_TOOLCHAIN_FILE among the options) needs a machine that runs its
# programs. The environment may set, for one:
#
#   LINUXGUI_ARCH   the program's architecture (default: this machine's)
#   LINUXGUI_GLIBC  the floor (default 2.34: Ubuntu 22.04, Debian 12,
#                   Fedora 35, RHEL 9, and later; 2.28 adds RHEL 8 and
#                   its rebuilds, Debian 10 and Ubuntu 18.10)
set -eu
src=$1
build=$2
out=$3
shift 3
arch=${LINUXGUI_ARCH:-$(uname -m)}
floor=GLIBC_${LINUXGUI_GLIBC:-2.34}

CC=clang-20 CXX=clang++-20 cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-s \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON "$@"
cmake --build "$build"
ctest --test-dir "$build" -R 'unit|selftest' --output-on-failure --no-tests=error

cache() {
    sed -n "s/^$1:[A-Z]*=//p" "$build/CMakeCache.txt"
}
v=$(cache CMAKE_PROJECT_VERSION)
edition=$(cache PROSPERO_EDITION)
exe=$build/prospero
said=$("$exe" --version)
# A build of an untagged commit adds " (git describe)"
case "$said" in
    "prospero $v"|"prospero $v ("*) ;;
    *) echo "build.sh: $exe --version says \"$said\"" >&2
       exit 1;;
esac

for lib in libwayland-client.so.0 libdecor-0.so.0 libX11.so.6; do
    if ! grep -qF "$lib" "$exe"; then
        echo "build.sh: the GUI lacks SDL's video through $lib" >&2
        exit 1
    fi
done
# SDL's D-Bus (the portal's file dialogs and theme, fcitx), which loads
# libdbus by name, and is built only where its headers are
if ! grep -qF org.freedesktop.portal.FileChooser "$exe"; then
    echo "build.sh: the GUI lacks SDL's D-Bus" >&2
    exit 1
fi
bad=$(readelf -dW "$exe" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' |
      grep -Ev '^(libc|libm|libdl|libpthread|librt|ld-linux[-a-z0-9_]*)\.so\.[0-9]+$' || true)
if [ -n "$bad" ]; then
    echo "build.sh: the GUI needs $bad" >&2
    exit 1
fi
max=$(readelf -VW "$exe" | grep -o 'GLIBC_2\.[0-9]*' | sort -uV | tail -1)
echo "newest: $max, floor $floor"
if [ "$(printf '%s\n' "$max" $floor | sort -V | tail -1)" != $floor ]; then
    echo "build.sh: the GUI needs $max" >&2
    exit 1
fi

case $edition in
community) readme=linux-README.txt ;;
itch)      readme=linux-README-itch.txt ;;
*) echo "build.sh: no README for the edition \"$edition\"" >&2; exit 1 ;;
esac
d=prospero-$v-linux-$arch
mkdir -p "$out/$d"
cp "$exe" "$src/assets/prospero.desktop" "$src/assets/prospero.png" "$out/$d/"
cp "$src/assets/$readme" "$out/$d/README.txt"
sh "$src/tools/licenses.sh" "$src" "$out/$d/LICENSES" gui
tar czf "$out/$d.tar.gz" -C "$out" --owner=0 --group=0 --numeric-owner "$d"
rm -r "${out:?}/${d:?}"
