cmake_minimum_required(VERSION 3.25)
project(harness VERSION 0.0.0 LANGUAGES C CXX)

# Unity build: each executable is one translation unit, a platform layer
# (src/main_*.cpp) that includes the portable core (src/harness.cpp). The
# core has no C++ runtime library, exceptions, or RTTI. Third-party code
# at the edges (GUI, HTTP) arrives in later milestones with its own flags.

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(HARNESS_NAME harness CACHE STRING "Executable name")
option(HARNESS_GUI "Build the GUI (OFF: the headless engine only)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Flags for the core translation units only, carried by an interface
# target so third-party libraries keep their own.
add_library(harness_core_flags INTERFACE)
target_compile_options(harness_core_flags INTERFACE
  -fno-exceptions -fno-rtti -fno-threadsafe-statics
  -fno-asynchronous-unwind-tables
  -Wall -Wextra -Wconversion -Wno-sign-conversion
  -Wno-unused-parameter -Wno-unused-function
)
target_compile_definitions(harness_core_flags INTERFACE
  HARNESS_VERSION="${PROJECT_VERSION}"
)

# Debug builds trap on undefined behavior, which includes failed asserts.
set(HARNESS_TRAP "")
check_cxx_compiler_flag(-fsanitize-trap HARNESS_HAS_TRAP)
if(HARNESS_HAS_TRAP)
  set(HARNESS_TRAP -fsanitize=undefined -fsanitize-trap)
else()
  check_cxx_compiler_flag(-fsanitize-undefined-trap-on-error HARNESS_HAS_TRAP2)
  if(HARNESS_HAS_TRAP2)
    set(HARNESS_TRAP -fsanitize=undefined -fsanitize-undefined-trap-on-error)
  endif()
endif()
target_compile_options(harness_core_flags INTERFACE
  "$<$<CONFIG:Debug>:-g3;${HARNESS_TRAP}>")
target_link_options(harness_core_flags INTERFACE
  "$<$<CONFIG:Debug>:${HARNESS_TRAP}>")

check_linker_flag(CXX -nostdlib++ HARNESS_HAS_NOSTDLIBXX)

if(HARNESS_GUI)
  include(FetchGui)
endif()

if(WIN32 AND HARNESS_GUI)
  # The GUI build has CRT startup, since FreeType, md4c and ImGui need a
  # CRT, though the core still never calls it. windows.h, D3D11 and the
  # ImGui Win32 backends stay in the gui_win32.cpp edge, which embeds
  # the manifest (assets/, through -Wa,-I).
  add_library(harness_gui OBJECT src/gui_win32.cpp)
  target_link_libraries(harness_gui PRIVATE imgui)
  target_compile_definitions(harness_gui PRIVATE HARNESS_GUIEDGE=1)
  target_compile_options(harness_gui PRIVATE
    -fno-exceptions -fno-rtti -Wa,-I${PROJECT_SOURCE_DIR}
  )
  add_executable(harness WIN32 src/main_win32.cpp)
  target_link_libraries(harness PRIVATE
    harness_gui imgui md4c kernel32 advapi32
  )
  target_link_options(harness PRIVATE -static -municode)
elseif(WIN32)
  # CRT-free: system DLLs only. Static libgcc keeps its helpers (e.g.
  # 64-bit division on i686) from adding a DLL dependency.
  add_executable(harness src/main_win32.cpp)
  target_link_options(harness PRIVATE -nostartfiles -static-libgcc)
  target_link_libraries(harness PRIVATE kernel32 advapi32)
else()
  find_package(Threads REQUIRED)
  add_executable(harness src/main_posix.cpp)
  target_link_libraries(harness PRIVATE Threads::Threads)
  if(HARNESS_GUI)
    target_link_libraries(harness PRIVATE imgui md4c)
  endif()
  if(HARNESS_HAS_NOSTDLIBXX)
    target_link_options(harness PRIVATE -nostdlib++)
  endif()
endif()
target_link_libraries(harness PRIVATE harness_core_flags)
if(HARNESS_GUI)
  target_compile_definitions(harness PRIVATE HARNESS_GUI=1)
endif()
if(APPLE AND HARNESS_GUI)
  # The same binary, also wrapped as an application bundle, which macOS
  # wants for a window of its own in the Dock and app switcher.
  add_custom_command(TARGET harness POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory Harness.app/Contents/MacOS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/assets/Info.plist Harness.app/Contents/Info.plist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:harness> Harness.app/Contents/MacOS/harness
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
  )
endif()
set_target_properties(harness PROPERTIES OUTPUT_NAME ${HARNESS_NAME})

if(NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_executable(harness_test src/main_test.cpp)
  target_link_libraries(harness_test PRIVATE harness_core_flags)
  if(HARNESS_GUI)
    # The GUI's pure modules: scheduler, markdown, layout, view model.
    target_link_libraries(harness_test PRIVATE md4c)
    target_compile_definitions(harness_test PRIVATE HARNESS_GUI=1)
  endif()
  if(HARNESS_HAS_NOSTDLIBXX)
    target_link_options(harness_test PRIVATE -nostdlib++)
  endif()
  add_test(NAME unit COMMAND harness_test)
  add_test(NAME selftest COMMAND harness --selftest)
endif()
