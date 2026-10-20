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

if(WIN32)
  # CRT-free: system DLLs only. Static libgcc keeps its helpers (e.g.
  # 64-bit division on i686) from adding a DLL dependency.
  add_executable(harness src/main_win32.cpp)
  target_link_options(harness PRIVATE -nostartfiles -static-libgcc)
  target_link_libraries(harness PRIVATE kernel32 advapi32)
else()
  find_package(Threads REQUIRED)
  add_executable(harness src/main_posix.cpp)
  target_link_libraries(harness PRIVATE Threads::Threads)
  if(HARNESS_HAS_NOSTDLIBXX)
    target_link_options(harness PRIVATE -nostdlib++)
  endif()
endif()
target_link_libraries(harness PRIVATE harness_core_flags)
set_target_properties(harness PROPERTIES OUTPUT_NAME ${HARNESS_NAME})

if(NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_executable(harness_test src/main_test.cpp)
  target_link_libraries(harness_test PRIVATE harness_core_flags)
  if(HARNESS_HAS_NOSTDLIBXX)
    target_link_options(harness_test PRIVATE -nostdlib++)
  endif()
  add_test(NAME unit COMMAND harness_test)
  add_test(NAME selftest COMMAND harness --selftest)
endif()
