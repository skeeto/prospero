# Cross-compile for x86-64 Linux with a GNU cross toolchain, as on an
# aarch64 Debian or Ubuntu host: the Linux x86-64 engine of
# tools/engines.py.
#
#   cmake -B build-x64 -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Linux-x86_64.cmake \
#         -DPROSPERO_GUI=OFF -DPROSPERO_STATIC=ON
#   cmake --build build-x64
#
# Host tooling: x86_64-linux-gnu-gcc and -g++ (apt: g++-x86-64-linux-gnu),
# whose libraries live under /usr/x86_64-linux-gnu.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_triple x86_64-linux-gnu)
set(CMAKE_C_COMPILER   ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)

set(CMAKE_FIND_ROOT_PATH /usr/${_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
