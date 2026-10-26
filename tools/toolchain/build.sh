#!/bin/sh
# Build release engines inside the toolchain image (Dockerfile here), as
# tools/engines.py --docker runs it:
#
#   sh build.sh SRC OUT PLATFORM...
#
# SRC is a source tree with its FetchContent sources in SRC/deps. OUT
# gets PLATFORM-engine.bin for each, and PLATFORM-desktop.bin for Linux,
# checked here: a static engine that runs (under QEMU) and a helper that
# needs nothing newer than glibc 2.28 for Linux, imports that XP loads
# for Windows. Any compiler warning fails the build.
set -eu
src=$1
out=$2
shift 2
mkdir -p "$out"

# Optimized for size, with unused sections dropped and no symbols: the
# engine waits on the network, and every byte is uploaded to each host
export CFLAGS="-ffunction-sections -fdata-sections"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-s -Wl,--gc-sections"

# build PLATFORM TARGET CMAKE-ARG...
build() {
    plat=$1
    target=$2
    shift 2
    b=/tmp/build-$plat-$target
    log=$out/$plat-$target.log
    echo "build.sh: $plat $target" >&2
    cmake -S "$src" -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release -DPROSPERO_GUI=OFF \
        "-DCMAKE_C_FLAGS_RELEASE=-Os -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-Os -DNDEBUG" \
        "$@" >$log 2>&1 || { tail -30 $log; exit 1; }
    cmake --build $b --target $target >>$log 2>&1 || { tail -30 $log; exit 1; }
    if grep 'warning:' $log; then
        echo "build.sh: $plat $target: warnings" >&2
        exit 1
    fi
}

for plat; do
    case $plat in
    linux-x86_64|linux-aarch64)
        arch=${plat#linux-}
        tc=-DCMAKE_TOOLCHAIN_FILE=$src/cmake/Toolchain-Linux-cross.cmake
        build $plat prospero $tc -DPROSPERO_TARGET=$arch-linux-musl -DPROSPERO_STATIC=ON
        exe=/tmp/build-$plat-prospero/prospero
        if file $exe | grep -qv 'statically linked'; then
            echo "build.sh: $plat: the engine is not static" >&2
            exit 1
        fi
        qemu-$arch $exe --version
        cp $exe $out/$plat-engine.bin

        # The helper is a target only of a static build
        build $plat prospero-desktop $tc -DPROSPERO_TARGET=$arch-linux-gnu \
            -DPROSPERO_STATIC=ON
        exe=/tmp/build-$plat-prospero-desktop/prospero-desktop
        # Every versioned glibc symbol it needs is in 2.28
        newer=$($arch-linux-gnu-objdump -T $exe \
                | grep -o 'GLIBC_[0-9.]*' | sort -u -V \
                | awk -F_ '{ split($2, v, "."); if (v[1] > 2 || v[2] > 28) print }')
        if [ -n "$newer" ]; then
            echo "build.sh: $plat: the helper needs $newer" >&2
            exit 1
        fi
        QEMU_LD_PREFIX=/opt/glibc/$arch-linux-gnu qemu-$arch $exe --version
        cp $exe $out/$plat-desktop.bin
        ;;
    windows-x86_64|windows-i686)
        tc=Toolchain-Mingw64.cmake
        [ $plat = windows-i686 ] && tc=Toolchain-Mingw32.cmake
        build $plat prospero -DCMAKE_TOOLCHAIN_FILE=$src/cmake/$tc
        exe=/tmp/build-$plat-prospero/prospero.exe
        python3 $src/tests/imports.py $exe
        cp $exe $out/$plat-engine.bin
        ;;
    *)
        echo "build.sh: unknown platform $plat" >&2
        exit 1
        ;;
    esac
done

# The licenses of what is linked into them: the toolchain's runtimes
# (in the image), this program's, and its FetchContent dependencies',
# from src/deps or else where a build fetched them
mkdir -p $out/LICENSES
cp /opt/licenses/* $out/LICENSES/
cp $src/UNLICENSE $out/LICENSES/prospero.txt
for dep in mbedtls cpp-httplib; do
    f=$(ls $src/deps/$dep/LICENSE* /tmp/build-*/_deps/$dep-src/LICENSE* 2>/dev/null | head -1)
    if [ -n "$f" ]; then
        cp $f $out/LICENSES/$dep.txt
    fi
done
# Wuffs, a single file, carries its notice in a comment (Apache-2.0 OR MIT)
f=$(ls $src/deps/wuffs/wuffs-*.c /tmp/build-*/_deps/wuffs-src/wuffs-*.c 2>/dev/null | head -1)
sed -n '/^\/\/ Copyright/,/^\/\/ SPDX-License-Identifier/s|^// \{0,1\}||p' $f \
    >$out/LICENSES/wuffs.txt
