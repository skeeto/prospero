# Cross-compile for 32-bit Windows with mingw-w64: the XP engine.
#
#   cmake -B build-xp -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw32.cmake \
#         -DHARNESS_GUI=OFF
#   cmake --build build-xp
#   ctest --test-dir build-xp
#
# Host tooling: i686-w64-mingw32-gcc, -g++, -objdump (brew: mingw-w64;
# apt: g++-mingw-w64-i686). The engine is CRT-free and imports only
# what Windows XP SP3 exports, which the imports test checks against
# tests/imports.py (needs uv). Native builds on XP with w64devkit's i686
# toolchain need no toolchain file.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(_triple i686-w64-mingw32)
set(CMAKE_C_COMPILER   ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
set(CMAKE_RC_COMPILER  ${_triple}-windres)
set(CMAKE_OBJDUMP      ${_triple}-objdump)

set(CMAKE_FIND_ROOT_PATH
  /usr/${_triple}
  /usr/${_triple}/sys-root/mingw
  /opt/homebrew/${_triple}
  /opt/homebrew/opt/mingw-w64/toolchain-i686
  /usr/local/${_triple}
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Third-party C++ at the edges (GUI builds) must not drag toolchain DLLs
# along: link libgcc and libstdc++ statically.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
