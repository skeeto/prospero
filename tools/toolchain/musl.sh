#!/bin/sh
# A GCC cross compiler for TARGET (e.g. x86_64-linux-musl) with musl as
# its C library, static only, installed in PREFIX with its sysroot at
# PREFIX/TARGET:
#
#   sh musl.sh TARGET PREFIX [GCC-CONFIGURE-FLAG ...]
#
# Sources are unpacked in /dl (binutils, gcc, musl), the musl patches
# already applied. The order is musl-cross-make's, less the steps a
# shared libc needs: binutils, musl's headers, the compiler alone, musl
# with that compiler, then GCC's target libraries (libgcc, libstdc++)
# against musl. The sysroot, inside PREFIX, is found relative to the
# installed compiler, so the build names it outright.
set -eu
target=$1
prefix=$2
shift 2
sysroot=$prefix/$target
jobs=$(nproc)
export PATH="$prefix/bin:$PATH"

mkdir -p /x/$target
cd /x/$target

mkdir binutils
(cd binutils && /dl/binutils/configure \
        --target=$target \
        --prefix=$prefix \
        --with-sysroot=$sysroot \
        --disable-nls \
        --disable-werror \
        --disable-multilib \
        --disable-gprofng \
        --enable-deterministic-archives \
     && make MAKEINFO=true -j$jobs \
     && make MAKEINFO=true install)

# musl's prefix is /usr, so that GCC finds it in the sysroot as it would
# any system's C library: headers in usr/include, libraries in usr/lib
make -C /dl/musl ARCH=${target%%-*} prefix=/usr DESTDIR=$sysroot install-headers

mkdir gcc
(cd gcc && /dl/gcc/configure \
        --target=$target \
        --prefix=$prefix \
        --with-sysroot=$sysroot \
        --with-build-sysroot=$sysroot \
        --enable-languages=c,c++ \
        --enable-static \
        --disable-shared \
        --disable-multilib \
        --disable-nls \
        --disable-bootstrap \
        --enable-threads=posix \
        --enable-tls \
        --enable-lto \
        --disable-libstdcxx-verbose \
        --disable-libsanitizer \
        --disable-libgomp \
        --disable-libquadmath \
        --disable-libitm \
        --disable-libssp \
        --disable-libvtv \
        --disable-werror \
        "$@" \
        CFLAGS="-O2" \
        CXXFLAGS="-O2" \
        CFLAGS_FOR_TARGET="${CFLAGS_FOR_TARGET:--O2}" \
        CXXFLAGS_FOR_TARGET="${CXXFLAGS_FOR_TARGET:--O2}" \
     && make -j$jobs all-gcc \
     && make install-gcc)

mkdir musl
(cd musl && /dl/musl/configure \
        --target=$target \
        --prefix=/usr \
        --disable-shared \
        CC=$target-gcc \
        CROSS_COMPILE=$target- \
        CFLAGS="-O2" \
     && make -j$jobs \
     && make DESTDIR=$sysroot install)

(cd gcc && make -j$jobs && make install-strip)

rm -rf /x/$target
