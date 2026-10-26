# The cross toolchains that build the release engines (tools/engines.py):
#
#   docker build -t prospero-toolchain tools/toolchain
#
# Every source is pinned by SHA-256, and the base image by digest, so the
# same file builds the same compilers here and in CI. GCC and binutils
# are w64devkit's versions, the compilers every Windows test has used.
#
#   /opt/musl/bin/x86_64-linux-musl-*    static musl, the Linux engines
#   /opt/musl/bin/aarch64-linux-musl-*
#   /opt/mingw64/bin/x86_64-w64-mingw32-*   the Windows engines
#   /opt/mingw32/bin/i686-w64-mingw32-*     (the XP engine)
#   /opt/glibc/bin/x86_64-linux-gnu-*       glibc 2.28, the Linux desktop
#   /opt/glibc/bin/aarch64-linux-gnu-*      helpers

FROM debian:trixie-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad AS base
RUN apt-get update && apt-get install --yes --no-install-recommends \
  build-essential ca-certificates cmake file libgmp-dev libmpc-dev \
  bzip2 libmpfr-dev m4 ninja-build patch python3 qemu-user xz-utils zip \
 && rm -rf /var/lib/apt/lists/*

FROM base AS dl
ARG BINUTILS_VERSION=2.47 \
    BINUTILS_SHA256=154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff \
    GCC_VERSION=16.2.0 \
    GCC_SHA256=e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e \
    MUSL_VERSION=1.2.6 \
    MUSL_SHA256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
WORKDIR /dl
ADD --checksum=sha256:$BINUTILS_SHA256 \
    https://ftp.wayne.edu/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz ./
ADD --checksum=sha256:$GCC_SHA256 \
    https://ftp.wayne.edu/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz ./
ADD --checksum=sha256:$MUSL_SHA256 \
    https://musl.libc.org/releases/musl-$MUSL_VERSION.tar.gz ./
# musl's security advisories through 1.2.6 (musl.libc.org), as posted
# to the mailing list, applied in series order
COPY patches/musl/ /dl/patches/musl/
RUN mkdir binutils gcc musl \
 && tar xJf binutils-$BINUTILS_VERSION.tar.xz -C binutils --strip-components=1 \
 && tar xJf gcc-$GCC_VERSION.tar.xz -C gcc --strip-components=1 \
 && tar xzf musl-$MUSL_VERSION.tar.gz -C musl --strip-components=1 \
 && for p in $(cat patches/musl/series); do \
        patch -d musl -p1 <patches/musl/$p || exit 1; \
    done \
 && rm *.tar.*

FROM dl AS musl-x86_64
COPY musl.sh /dl/
RUN sh /dl/musl.sh x86_64-linux-musl /opt/musl

FROM dl AS musl-aarch64
COPY musl.sh /dl/
# Cortex-A53 erratum 843419 (Raspberry Pi 3 and others), as Debian does
RUN sh /dl/musl.sh aarch64-linux-musl /opt/musl --enable-fix-cortex-a53-843419

# glibc 2.28 for the desktop helpers: Debian 10's packages (buster),
# whose hashes come from its signed archive index
FROM dl AS dl-glibc
ARG DEB=https://archive.debian.org/debian/pool/main
WORKDIR /dl/deb
ADD --checksum=sha256:0ee0f0bfcd7b59ffda1dbc76ef58fa79512913204f2a336206467bfbf45bc69f \
    $DEB/g/glibc/libc6_2.28-10+deb10u1_amd64.deb ./
ADD --checksum=sha256:76b8413920d91a9a4289f99839933d0d30095e0ec12b05bb4f1a7f27e5ef4a55 \
    $DEB/g/glibc/libc6-dev_2.28-10+deb10u1_amd64.deb ./
ADD --checksum=sha256:c512f30f5abe413464b79ed23b54958df8cc104446c3ae93d83d28e23728a571 \
    $DEB/l/linux/linux-libc-dev_4.19.249-2_amd64.deb ./
ADD --checksum=sha256:09f5e92b1527cdc06b757499bb4d97ddf1b064fc3d163f978e5de18b131cd330 \
    $DEB/g/glibc/libc6_2.28-10+deb10u1_arm64.deb ./
ADD --checksum=sha256:04097716018288cef8b82339faa1a9847838b89f39574b5b169f4657da8bf269 \
    $DEB/g/glibc/libc6-dev_2.28-10+deb10u1_arm64.deb ./
ADD --checksum=sha256:e9a92d71d62655f673433c345a03fecaa4292f93c6572b28fd3d16dcfd1cc7f6 \
    $DEB/l/linux/linux-libc-dev_4.19.249-2_arm64.deb ./
WORKDIR /dl

FROM dl-glibc AS glibc-x86_64
COPY glibc.sh /dl/
RUN sh /dl/glibc.sh x86_64-linux-gnu /opt/glibc deb/*_amd64.deb

FROM dl-glibc AS glibc-aarch64
COPY glibc.sh /dl/
RUN sh /dl/glibc.sh aarch64-linux-gnu /opt/glibc deb/*_arm64.deb \
        -- --enable-fix-cortex-a53-843419

# The same GCC and binutils, with w64devkit's patch queues (v2.10.1),
# and mingw-w64 as w64devkit builds it
FROM dl AS dl-mingw
ARG MINGW_VERSION=14.0.0 \
    MINGW_SHA256=6eaf921d9eb987d3820b364ea9775bc19b965ec81490b6fdd716526c28e1995c
ADD --checksum=sha256:$MINGW_SHA256 \
    https://downloads.sourceforge.net/project/mingw-w64/mingw-w64/mingw-w64-release/mingw-w64-v$MINGW_VERSION.tar.bz2 ./
COPY patches/w64devkit/ /dl/patches/w64devkit/
RUN mkdir mingw \
 && tar xjf mingw-w64-v$MINGW_VERSION.tar.bz2 -C mingw --strip-components=1 \
 && rm mingw-w64-v$MINGW_VERSION.tar.bz2 \
 && for q in binutils gcc mingw; do \
        for p in $(cat patches/w64devkit/$q/series); do \
            patch -d $q -p1 <patches/w64devkit/$q/$p || exit 1; \
        done; \
    done \
 && sed -ri 's/(static bool insert_timestamp = )/\1!/' binutils/ld/emultempl/pe*.em \
 && sed -ri 's/(int pe_enable_stdcall_fixup = )/\1!!/' binutils/ld/emultempl/pe*.em \
 && sed -i /OpenThreadToken/d mingw/mingw-w64-crt/lib32/kernel32.def \
 && printf '#include <crtdefs.h>\n#if __has_include_next(<stddef.h>)\n#include_next <stddef.h>\n#endif\n' \
      >mingw/mingw-w64-headers/crt/stddef.h

FROM dl-mingw AS mingw-x86_64
COPY mingw.sh /dl/
RUN sh /dl/mingw.sh x86_64-w64-mingw32 /opt/mingw64

# The XP engine: Pentium 4, and no UTF-8 manifest, as w64devkit's x86
FROM dl-mingw AS mingw-i686
COPY mingw.sh /dl/
RUN sh /dl/mingw.sh i686-w64-mingw32 /opt/mingw32 \
        --with-arch=pentium4 --disable-win32-utf8-manifest

FROM base
COPY --from=musl-x86_64 /opt/musl /opt/musl
COPY --from=musl-aarch64 /opt/musl /opt/musl
COPY --from=mingw-x86_64 /opt/mingw64 /opt/mingw64
COPY --from=mingw-i686 /opt/mingw32 /opt/mingw32
COPY --from=glibc-x86_64 /opt/glibc /opt/glibc
COPY --from=glibc-aarch64 /opt/glibc /opt/glibc
# What the engines link statically: musl, and GCC's runtime (libgcc,
# libstdc++) under its exception to the GPL
COPY --from=dl /dl/musl/COPYRIGHT /opt/licenses/musl.txt
COPY --from=dl /dl/gcc/COPYING.RUNTIME /opt/licenses/gcc-runtime-exception.txt
COPY --from=dl /dl/gcc/COPYING3 /opt/licenses/gcc-gpl-3.0.txt
ENV PATH="/opt/musl/bin:/opt/mingw64/bin:/opt/mingw32/bin:/opt/glibc/bin:${PATH}"
