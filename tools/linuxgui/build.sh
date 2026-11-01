#!/bin/sh
# Build, test, check and pack the Linux GUI, where install.sh set up
# the tools: the release's linux-gui job (the Community Edition) and
# tools/itch.py (in the Dockerfile's image, -DPROSPERO_EDITION=itch).
#
#   sh tools/linuxgui/build.sh SRC BUILD OUT [CMAKE_OPTION ...]
#
# SRC is an unpacked source tarball (tools/dist.py), BUILD a build
# directory, and OUT gets prospero-VERSION-linux-ARCH.tar.gz: the
# program, its desktop file and icon, a README.txt for its edition,
# and LICENSES. The program must need only glibc's own libraries, and
# none of their symbols past GLIBC_2.34 (Ubuntu 22.04, Debian 12,
# Fedora 35, RHEL 10, and later), and must have SDL's Wayland and X11
# video, which load their libraries by name.
set -eu
src=$1
build=$2
out=$3
shift 3

CC=clang-20 CXX=clang++-20 cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-s \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON "$@"
cmake --build "$build"
ctest --test-dir "$build" -R 'unit|selftest' --output-on-failure

cache() {
    sed -n "s/^$1:[A-Z]*=//p" "$build/CMakeCache.txt"
}
v=$(cache CMAKE_PROJECT_VERSION)
edition=$(cache PROSPERO_EDITION)
exe=$build/prospero
said=$("$exe" --version)
if [ "$said" != "prospero $v" ]; then
    echo "build.sh: $exe --version says \"$said\"" >&2
    exit 1
fi

for lib in libwayland-client.so.0 libdecor-0.so.0 libX11.so.6; do
    if ! grep -qF "$lib" "$exe"; then
        echo "build.sh: the GUI lacks SDL's video through $lib" >&2
        exit 1
    fi
done
bad=$(readelf -dW "$exe" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' |
      grep -Ev '^(libc|libm|libdl|libpthread|librt|ld-linux[-a-z0-9_]*)\.so\.[0-9]+$' || true)
if [ -n "$bad" ]; then
    echo "build.sh: the GUI needs $bad" >&2
    exit 1
fi
max=$(readelf -VW "$exe" | grep -o 'GLIBC_2\.[0-9]*' | sort -uV | tail -1)
echo "newest: $max"
if [ "$(printf '%s\n' "$max" GLIBC_2.34 | sort -V | tail -1)" != GLIBC_2.34 ]; then
    echo "build.sh: the GUI needs $max" >&2
    exit 1
fi

case $edition in
community) readme=linux-README.txt ;;
itch)      readme=linux-README-itch.txt ;;
*) echo "build.sh: no README for the edition \"$edition\"" >&2; exit 1 ;;
esac
d=prospero-$v-linux-$(uname -m)
mkdir -p "$out/$d"
cp "$exe" "$src/assets/prospero.desktop" "$src/assets/prospero.png" "$out/$d/"
cp "$src/assets/$readme" "$out/$d/README.txt"
sh "$src/tools/licenses.sh" "$src" "$out/$d/LICENSES" gui
tar czf "$out/$d.tar.gz" -C "$out" --owner=0 --group=0 --numeric-owner "$d"
rm -r "${out:?}/${d:?}"
