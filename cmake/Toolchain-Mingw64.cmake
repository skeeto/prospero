# Cross-compile for 64-bit Windows with mingw-w64.
#
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw64.cmake
#   cmake --build build-win
#
# Host tooling: x86_64-w64-mingw32-gcc, -g++, -windres (brew: mingw-w64;
# apt: g++-mingw-w64-x86-64). Native Windows builds with w64devkit need
# no toolchain file.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
set(CMAKE_RC_COMPILER  ${_triple}-windres)

set(CMAKE_FIND_ROOT_PATH
  /usr/${_triple}
  /usr/${_triple}/sys-root/mingw
  /opt/homebrew/${_triple}
  /opt/homebrew/opt/mingw-w64/toolchain-x86_64
  /usr/local/${_triple}
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Third-party C++ at the edges (GUI builds) must not drag toolchain DLLs
# along: link libgcc and libstdc++ statically.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
