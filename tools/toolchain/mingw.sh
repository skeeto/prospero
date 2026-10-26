#!/bin/sh
# A mingw-w64 GCC cross compiler for TARGET (x86_64-w64-mingw32 or
# i686-w64-mingw32), installed in PREFIX with its sysroot there too:
#
#   sh mingw.sh TARGET PREFIX [GCC-CONFIGURE-FLAG ...]
#
# Sources are unpacked in /dl (binutils, gcc, mingw), w64devkit's patch
# queues already applied. The steps and options are the cross stage of
# w64devkit's Dockerfile (v2.10.1) for one target, without multilib,
# Fortran or libgomp, and with LTO.
set -eu
target=$1
prefix=$2
shift 2
jobs=$(nproc)
export PATH="$prefix/bin:$PATH"

mkdir -p /x/$target
cd /x/$target

mkdir binutils
(cd binutils && /dl/binutils/configure \
        --target=$target \
        --prefix=$prefix \
        --with-sysroot=$prefix \
        --disable-nls \
        --disable-werror \
        --disable-multilib \
        --disable-gprofng \
        --enable-deterministic-archives \
     && make MAKEINFO=true -j$jobs \
     && make MAKEINFO=true install)

mkdir headers
(cd headers && /dl/mingw/mingw-w64-headers/configure \
        --prefix=$prefix/$target \
        --host=$target \
        --with-default-msvcrt=msvcrt-os \
     && make -j$jobs \
     && make install)
# GCC looks for the sysroot's headers and libraries in PREFIX/mingw
ln -sfn $target $prefix/mingw

lib32=disable lib64=enable
case $target in i686-*) lib32=enable lib64=disable ;; esac

mkdir gcc
(cd gcc && /dl/gcc/configure \
        --target=$target \
        --prefix=$prefix \
        --with-sysroot=$prefix \
        --enable-languages=c,c++ \
        --enable-static \
        --disable-shared \
        --with-pic \
        --disable-multilib \
        --disable-nls \
        --disable-bootstrap \
        --enable-threads=posix \
        --enable-tls \
        --enable-lto \
        --disable-libstdcxx-verbose \
        --disable-libgomp \
        --disable-dependency-tracking \
        --disable-werror \
        "$@" \
        CFLAGS="-O2" \
        CXXFLAGS="-O2" \
        CFLAGS_FOR_TARGET="${CFLAGS_FOR_TARGET:--O2}" \
        CXXFLAGS_FOR_TARGET="${CXXFLAGS_FOR_TARGET:--O2}" \
     && make -j$jobs all-gcc \
     && make install-gcc)

mkdir crt
(cd crt && /dl/mingw/mingw-w64-crt/configure \
        --prefix=$prefix/$target \
        --with-sysroot=$prefix \
        --host=$target \
        --with-default-msvcrt=msvcrt-os \
        --disable-dependency-tracking \
        --$lib32-lib32 \
        --$lib64-lib64 \
        CFLAGS="-O2" \
     && make -j$jobs \
     && make install)

mkdir winpthreads
(cd winpthreads && /dl/mingw/mingw-w64-libraries/winpthreads/configure \
        --prefix=$prefix/$target \
        --with-sysroot=$prefix \
        --host=$target \
        --enable-static \
        --disable-shared \
        CFLAGS="-O2" \
     && make -j$jobs \
     && make install)

(cd gcc && make -j$jobs && make install-strip)

rm -rf /x/$target
