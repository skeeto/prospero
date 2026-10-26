# Cross-compile for Linux with a GCC cross toolchain named by its target,
# as in the image tools/toolchain/Dockerfile builds: static musl engines
# (/opt/musl) and glibc 2.28 desktop helpers (/opt/glibc) of a release.
#
#   cmake -B build-musl -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Linux-cross.cmake \
#         -DPROSPERO_TARGET=aarch64-linux-musl \
#         -DPROSPERO_GUI=OFF -DPROSPERO_STATIC=ON
#
# PROSPERO_TARGET defaults to x86_64-linux-musl. The compilers are found
# on PATH as TARGET-gcc and TARGET-g++, each with its own sysroot.

set(PROSPERO_TARGET x86_64-linux-musl CACHE STRING "GCC cross target")
# Toolchain files run again for try_compile projects, which see only
# these variables
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PROSPERO_TARGET)

set(CMAKE_SYSTEM_NAME Linux)
string(REGEX REPLACE "-.*" "" CMAKE_SYSTEM_PROCESSOR ${PROSPERO_TARGET})

set(CMAKE_C_COMPILER   ${PROSPERO_TARGET}-gcc)
set(CMAKE_CXX_COMPILER ${PROSPERO_TARGET}-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
