# Cross-compile for Linux with a GCC cross toolchain named by its target,
# as in the image tools/toolchain/Dockerfile builds: static musl engines
# (/opt/musl), and glibc 2.28 desktop helpers and the itch.io edition's
# GUI (/opt/glibc) of a release.
#
#   cmake -B build-musl -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Linux-cross.cmake \
#         -DPROSPERO_TARGET=aarch64-linux-musl \
#         -DPROSPERO_GUI=OFF -DPROSPERO_STATIC=ON
#
# PROSPERO_TARGET defaults to x86_64-linux-musl. The compilers are found
# on PATH as TARGET-gcc and TARGET-g++, each with its own sysroot. For
# glibc, CMake finds libraries and pkg-config finds packages there (the
# GUI's, which SDL builds against), and the tests are built, to run
# where this machine runs the target's programs: natively, or through
# binfmt_misc with the sysroot's loader and libraries at their paths
# (as tools/itch.py arranges). Not QEMU as an emulator: the selftest
# runs programs of its own, which only binfmt_misc would emulate.

set(PROSPERO_TARGET x86_64-linux-musl CACHE STRING "GCC cross target")
# Toolchain files run again for try_compile projects, which see only
# these variables
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PROSPERO_TARGET)

set(CMAKE_SYSTEM_NAME Linux)
string(REGEX REPLACE "-.*" "" CMAKE_SYSTEM_PROCESSOR ${PROSPERO_TARGET})

set(CMAKE_C_COMPILER   ${PROSPERO_TARGET}-gcc)
set(CMAKE_CXX_COMPILER ${PROSPERO_TARGET}-g++)

# The glibc targets' sysroot (the musl engines are left as they were)
if(PROSPERO_TARGET MATCHES "-linux-gnu$")
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-sysroot
                  OUTPUT_VARIABLE PROSPERO_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(PROSPERO_SYSROOT)
  set(CMAKE_SYSROOT ${PROSPERO_SYSROOT})
  set(ENV{PKG_CONFIG_SYSROOT_DIR} ${PROSPERO_SYSROOT})
  set(ENV{PKG_CONFIG_LIBDIR}
      "${PROSPERO_SYSROOT}/usr/lib/${PROSPERO_TARGET}/pkgconfig:${PROSPERO_SYSROOT}/usr/share/pkgconfig")
  # Their tests are built (CMakeLists.txt), for a machine that runs them
  set(PROSPERO_CROSS_TESTS ON)
  # The GUI's libraries are newer than the sysroot's glibc, and SDL's
  # checks link against them, though it loads them only at run time:
  # their own references are left to the system that loads them. The
  # program's own still must resolve, and build.sh checks what it needs.
  set(CMAKE_C_STANDARD_LIBRARIES_INIT -Wl,--allow-shlib-undefined)
  set(CMAKE_CXX_STANDARD_LIBRARIES_INIT -Wl,--allow-shlib-undefined)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
