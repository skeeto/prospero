# Cross-compile for Windows on Linux with w64devkit's own toolchain, run
# under Wine: how the release workflow builds the Windows programs
# (.github/workflows/release.yml), since a distribution's Mingw-w64 may
# predate #embed (GCC 15).
#
#   cmake -B build-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-W64devkit-Wine.cmake \
#         -DW64DEVKIT=$HOME/w64devkit
#   cmake --build build-win
#
# W64DEVKIT (or $W64DEVKIT) is an unpacked x64 w64devkit, 2.10 or later,
# whose GCC is multilib: -DW64DEVKIT_ARCH=i686 builds 32-bit programs
# with its -m32, which with -DPROSPERO_GUI=OFF is the XP engine. Host
# tooling: a Wine that runs 64-bit programs.
#
# CMake needs a compiler it can run as a program, so each tool gets a
# small script, written under the build directory, that runs it under
# Wine. The paths CMake passes are Unix paths, which Wine resolves on the
# current drive, Z:, the root of the file system.

set(CMAKE_SYSTEM_NAME Windows)

if(NOT W64DEVKIT)
  set(W64DEVKIT "$ENV{W64DEVKIT}")
endif()
if(NOT W64DEVKIT_ARCH)
  set(W64DEVKIT_ARCH x86_64)
endif()
if(NOT EXISTS "${W64DEVKIT}/bin/gcc.exe")
  message(FATAL_ERROR "W64DEVKIT must name an unpacked w64devkit (no "
                      "${W64DEVKIT}/bin/gcc.exe)")
endif()
if(W64DEVKIT_ARCH STREQUAL "x86_64")
  set(_m "")
  set(_rc "")
elseif(W64DEVKIT_ARCH STREQUAL "i686")
  set(_m " -m32")
  set(_rc " -F pe-i386")
else()
  message(FATAL_ERROR "W64DEVKIT_ARCH is x86_64 or i686, not ${W64DEVKIT_ARCH}")
endif()
set(CMAKE_SYSTEM_PROCESSOR ${W64DEVKIT_ARCH})

# try_compile projects read this file again, in a build directory of
# their own, so they take these from the main project
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  W64DEVKIT W64DEVKIT_ARCH W64DEVKIT_WINE W64DEVKIT_WRAPPERS)
if(NOT W64DEVKIT_WINE)
  find_program(W64DEVKIT_WINE NAMES wine wine64 wine-stable REQUIRED
               NO_CMAKE_FIND_ROOT_PATH)
endif()
if(NOT W64DEVKIT_WRAPPERS)
  set(W64DEVKIT_WRAPPERS "${CMAKE_BINARY_DIR}/w64devkit-wine")
endif()

# Quiet Wine's own diagnostics, which CMake's flag checks would read as
# the compiler's, unless WINEDEBUG asks for them.
function(w64devkit_wrap tool extra post)
  string(REPLACE "'" "'\\''" wine "${W64DEVKIT_WINE}")
  string(REPLACE "'" "'\\''" exe "${W64DEVKIT}/bin/${tool}.exe")
  file(CONFIGURE OUTPUT "${W64DEVKIT_WRAPPERS}/${tool}" CONTENT
    "#!/bin/sh\nWINEDEBUG=\${WINEDEBUG--all} '${wine}' '${exe}'${extra} \"\$@\" || exit\n${post}"
    @ONLY)
  file(CHMOD "${W64DEVKIT_WRAPPERS}/${tool}" FILE_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)
endfunction()

# GCC writes into its dependency files some paths as Wine resolved them,
# on Z: (#embed's, and the system headers'), which turn back into Unix
# paths here, so that Ninja and Make find those files and an unchanged
# build has nothing to do.
set(_deps [=[for a do
    if [ "$prev" = -MF ] && [ -f "$a" ]; then
        sed 's|Z:/|/|g' "$a" >"$a.unix" && mv "$a.unix" "$a" || exit
    fi
    prev=$a
done
]=])
foreach(tool IN ITEMS ar nm objdump ranlib strip)
  w64devkit_wrap(${tool} "" "")
endforeach()
w64devkit_wrap(gcc "${_m}" "${_deps}")
w64devkit_wrap(g++ "${_m}" "${_deps}")
w64devkit_wrap(windres "${_rc}" "")

set(CMAKE_C_COMPILER   ${W64DEVKIT_WRAPPERS}/gcc)
set(CMAKE_CXX_COMPILER ${W64DEVKIT_WRAPPERS}/g++)
set(CMAKE_RC_COMPILER  ${W64DEVKIT_WRAPPERS}/windres)
set(CMAKE_AR           ${W64DEVKIT_WRAPPERS}/ar)
set(CMAKE_RANLIB       ${W64DEVKIT_WRAPPERS}/ranlib)
set(CMAKE_NM           ${W64DEVKIT_WRAPPERS}/nm)
set(CMAKE_OBJDUMP      ${W64DEVKIT_WRAPPERS}/objdump)
set(CMAKE_STRIP        ${W64DEVKIT_WRAPPERS}/strip)

set(CMAKE_FIND_ROOT_PATH ${W64DEVKIT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
