#!/bin/sh
# A GCC cross compiler for TARGET (x86_64-linux-gnu or aarch64-linux-gnu)
# against glibc 2.28, installed in PREFIX with its sysroot at
# PREFIX/TARGET:
#
#   sh glibc.sh TARGET PREFIX DEB... [-- GCC-CONFIGURE-FLAG ...]
#
# The sysroot is Debian 10's glibc and kernel headers, unpacked from the
# given packages, not glibc built here (2.28 predates the compilers that
# would build it). Programs linked against it run on glibc 2.28 and
# later: the Linux desktop helpers, which load X11 and portal libraries
# at run time and so cannot be static. Sources are unpacked in /dl
# (binutils, gcc).
set -eu
target=$1
prefix=$2
shift 2
sysroot=$prefix/$target
jobs=$(nproc)
export PATH="$prefix/bin:$PATH"

mkdir -p $sysroot
while [ $# -gt 0 ] && [ "$1" != -- ]; do
    dpkg-deb -x "$1" $sysroot
    shift
done
[ $# -gt 0 ] && shift
# Debian's absolute symlinks (e.g. /lib64/ld-linux-x86-64.so.2) point
# into the host: make them point within the sysroot
find $sysroot -type l | while read -r link; do
    to=$(readlink "$link")
    case $to in /*) ln -sfn "$sysroot$to" "$link" ;; esac
done

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

# Debian's multiarch layout: libraries in lib/TARGET and usr/lib/TARGET,
# and some headers in usr/include/TARGET
mkdir gcc
(cd gcc && /dl/gcc/configure \
        --target=$target \
        --prefix=$prefix \
        --with-sysroot=$sysroot \
        --with-build-sysroot=$sysroot \
        --with-glibc-version=2.28 \
        --enable-multiarch \
        --enable-languages=c,c++ \
        --enable-static \
        --disable-shared \
        --disable-multilib \
        --disable-nls \
        --disable-bootstrap \
        --enable-threads=posix \
        --enable-tls \
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
     && make -j$jobs \
     && make install-strip)

rm -rf /x/$target
