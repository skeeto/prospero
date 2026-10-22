cmake_minimum_required(VERSION 3.25)
project(prospero VERSION 0.0.0 LANGUAGES C CXX)

# Unity build: each executable is one translation unit, a platform layer
# (src/main_*.cpp) that includes the portable core (src/prospero.cpp). The
# core has no C++ runtime library, exceptions, or RTTI. Third-party code
# at the edges (GUI, HTTP) arrives in later milestones with its own flags.

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(PROSPERO_NAME prospero CACHE STRING "Executable name")
option(PROSPERO_GUI "Build the GUI (OFF: the headless engine only)" ON)
option(PROSPERO_STATIC "Link fully statically (Linux: deployed engines)" OFF)

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
add_library(prospero_core_flags INTERFACE)
target_compile_options(prospero_core_flags INTERFACE
  -fno-exceptions -fno-rtti -fno-threadsafe-statics
  -fno-asynchronous-unwind-tables
  -Wall -Wextra -Wconversion -Wno-sign-conversion
  -Wno-unused-parameter -Wno-unused-function
)
target_compile_definitions(prospero_core_flags INTERFACE
  PROSPERO_VERSION="${PROJECT_VERSION}"
)

# Debug builds trap on undefined behavior, which includes failed asserts,
# and optimize the core a little (-Og), for speed nearer Release's on
# slow machines, at some cost in a debugger.
set(PROSPERO_TRAP "")
check_cxx_compiler_flag(-fsanitize-trap PROSPERO_HAS_TRAP)
if(PROSPERO_HAS_TRAP)
  set(PROSPERO_TRAP -fsanitize=undefined -fsanitize-trap)
else()
  check_cxx_compiler_flag(-fsanitize-undefined-trap-on-error PROSPERO_HAS_TRAP2)
  if(PROSPERO_HAS_TRAP2)
    set(PROSPERO_TRAP -fsanitize=undefined -fsanitize-undefined-trap-on-error)
  endif()
endif()
if(PROSPERO_TRAP AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
  # The i386 ABI aligns 8-byte integers to 4 on the stack, and so does
  # GCC, whose alignment check then demands 8 of its own locals.
  list(APPEND PROSPERO_TRAP -fno-sanitize=alignment)
endif()
target_compile_options(prospero_core_flags INTERFACE
  "$<$<CONFIG:Debug>:-g3;-Og;${PROSPERO_TRAP}>")
target_link_options(prospero_core_flags INTERFACE
  "$<$<CONFIG:Debug>:${PROSPERO_TRAP}>")

check_linker_flag(CXX -nostdlib++ PROSPERO_HAS_NOSTDLIBXX)

# __FILE__, which ImGui's assertions log even in Release (src/imconfig.h),
# names files relative to the source tree, dependencies' too, so that no
# build directory is left in a program, and the same sources build the
# same program wherever they are unpacked.
check_cxx_compiler_flag(-fmacro-prefix-map=a=b PROSPERO_HAS_PREFIXMAP)
if(PROSPERO_HAS_PREFIXMAP)
  add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${PROJECT_SOURCE_DIR}/=>")
endif()

# The core embeds the seed configuration with #embed (config.cpp), which
# not every compiler lists as a dependency.
set_property(SOURCE src/main_posix.cpp src/main_win32.cpp src/main_test.cpp
  APPEND PROPERTY OBJECT_DEPENDS ${PROJECT_SOURCE_DIR}/assets/prospero.ini)

if(PROSPERO_GUI)
  include(FetchGui)
endif()

if(WIN32 AND PROSPERO_GUI)
  # The GUI build has CRT startup, since FreeType, md4c and ImGui need a
  # CRT, though the core still never calls it. windows.h, D3D11 and the
  # ImGui Win32 backends stay in the gui_win32.cpp edge, which embeds
  # the manifest (assets/, through -Wa,-I).
  add_library(prospero_gui OBJECT src/gui_win32.cpp)
  target_link_libraries(prospero_gui PRIVATE imgui)
  target_compile_definitions(prospero_gui PRIVATE PROSPERO_GUIEDGE=1)
  target_compile_options(prospero_gui PRIVATE
    -fno-exceptions -fno-rtti -Wa,-I${PROJECT_SOURCE_DIR}
  )
  add_executable(prospero WIN32 src/main_win32.cpp)
  target_link_libraries(prospero PRIVATE
    prospero_gui imgui md4c kernel32 advapi32 winhttp
  )
  target_link_options(prospero PRIVATE -static -municode)
elseif(WIN32)
  # CRT-free: system DLLs only. Static libgcc keeps its helpers (e.g.
  # 64-bit division on i686) from adding a DLL dependency.
  add_executable(prospero src/main_win32.cpp)
  target_link_options(prospero PRIVATE -nostartfiles -static-libgcc)
  target_link_libraries(prospero PRIVATE kernel32 advapi32 winhttp)
else()
  # HTTP goes through the httpedge object, the one translation unit
  # with the C++ standard library (and so libc++/libstdc++ at link).
  include(FetchNet)
  find_package(Threads REQUIRED)
  add_library(httpedge OBJECT src/http_httplib.cpp)
  target_link_libraries(httpedge PUBLIC httpnet)
  target_compile_options(httpedge PRIVATE -fno-exceptions)
  add_executable(prospero src/main_posix.cpp)
  target_link_libraries(prospero PRIVATE httpedge Threads::Threads)
  if(PROSPERO_GUI)
    target_link_libraries(prospero PRIVATE imgui md4c)
  endif()
  # Every tool process starts under a reaper, this executable, so its
  # startup is on the critical path: loading libstdc++ dynamically costs
  # a millisecond or two per tool on a Raspberry Pi.
  check_linker_flag(CXX -static-libstdc++ PROSPERO_HAS_STATICXX)
  if(PROSPERO_HAS_STATICXX AND NOT APPLE)
    target_link_options(prospero PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  if(PROSPERO_STATIC AND NOT APPLE)
    # An engine deployed to other machines (tools/engines.py) cannot
    # know their C library, so it brings its own.
    target_link_options(prospero PRIVATE -static)
  endif()
endif()
target_link_libraries(prospero PRIVATE prospero_core_flags)
if(WIN32)
  # A zero PE timestamp, as w64devkit's ld writes by default, rather than
  # the link time, so the same sources link the same program with any
  # GNU toolchain. Releases check it (tools/pecheck.py).
  target_link_options(prospero PRIVATE -Wl,--no-insert-timestamp)
endif()
if(PROSPERO_GUI)
  target_compile_definitions(prospero PRIVATE PROSPERO_GUI=1)
endif()
if(APPLE AND PROSPERO_GUI)
  # The same binary, also wrapped as an application bundle, which macOS
  # wants for a window of its own in the Dock and app switcher.
  add_custom_command(TARGET prospero POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory Prospero.app/Contents/MacOS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/assets/Info.plist Prospero.app/Contents/Info.plist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:prospero> Prospero.app/Contents/MacOS/prospero
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
  )
endif()
set_target_properties(prospero PROPERTIES OUTPUT_NAME ${PROSPERO_NAME})

find_program(UV uv)
if(WIN32 AND NOT PROSPERO_GUI AND UV)
  # The headless engine, the XP engine as an i686 build, imports only
  # what Windows XP SP3 exports. A host check, so cross builds run it.
  enable_testing()
  add_test(NAME imports COMMAND ${UV} run
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/imports.py $<TARGET_FILE:prospero>)
endif()

if(NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_executable(prospero_test src/main_test.cpp)
  target_link_libraries(prospero_test PRIVATE prospero_core_flags)
  if(PROSPERO_GUI)
    # The GUI's pure modules: scheduler, markdown, layout, view model.
    target_link_libraries(prospero_test PRIVATE md4c)
    target_compile_definitions(prospero_test PRIVATE PROSPERO_GUI=1)
  endif()
  if(PROSPERO_HAS_NOSTDLIBXX)
    target_link_options(prospero_test PRIVATE -nostdlib++)
  endif()
  add_test(NAME unit COMMAND prospero_test)
  if(APPLE)
    # The HTTP edge with the test-only calls of its trust evaluation,
    # which asks the system's (Security.framework) about each chain
    add_executable(prospero_edgetest src/main_edgetest.cpp)
    target_link_libraries(prospero_edgetest PRIVATE httpnet Threads::Threads)
    target_compile_options(prospero_edgetest PRIVATE -fno-exceptions)
    if(UV)
      add_test(NAME sectrust COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/sectrust.py
        --edgetest $<TARGET_FILE:prospero_edgetest>)
    endif()
  endif()
  add_test(NAME selftest COMMAND prospero --selftest)
  # It measures latencies and the reaper's start, so it runs alone
  set_tests_properties(selftest PROPERTIES RUN_SERIAL ON)
  # The engine end to end against tests/fake_provider.py
  if(UV)
    add_test(NAME scenarios COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios.py --prospero $<TARGET_FILE:prospero>)
    if(APPLE)
      # Its selftest with the harness disclaimed measures latencies too,
      # so it runs alone, apart from cleanup's cases
      add_test(NAME cleanup COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cleanup.py --prospero $<TARGET_FILE:prospero>
        --no-selftest)
      add_test(NAME selftest_disclaimed COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cleanup.py --prospero $<TARGET_FILE:prospero>
        --selftest-only)
      set_tests_properties(selftest_disclaimed PROPERTIES RUN_SERIAL ON)
    else()
      add_test(NAME cleanup COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cleanup.py --prospero $<TARGET_FILE:prospero>)
    endif()
    # Hosts and their clients on this machine (the ssh ones take --ssh)
    add_test(NAME remote COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/remote.py --prospero $<TARGET_FILE:prospero>)
    # URL includes in the configuration, against a fake web server
    add_test(NAME includes COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/includes.py --prospero $<TARGET_FILE:prospero>)
    # Deploying engines through a fake ssh and scp (a real host: --ssh)
    add_test(NAME deploy COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/deploy.py --prospero $<TARGET_FILE:prospero>)
    # The release's check of Authenticode signatures (tools/pecheck.py),
    # on a program signed with a throwaway key
    find_program(OPENSSL openssl)
    if(OPENSSL)
      add_test(NAME signcheck COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/signcheck.py)
    endif()
    # The suites bound how long the harness takes, which a machine kept
    # busy by other suites can miss through no fault of the harness's: a
    # Raspberry Pi's SD card stalls for seconds under writes from several
    # at once. ctest -j N runs tests together only as far as their
    # processor slots fit in N, so each claims what it keeps busy: cases
    # at once (scenarios, includes), or copies of the engine (deploy).
    # A quad-core Pi at -j4 then runs them one or two at a time, and a
    # bigger machine several.
    set_tests_properties(scenarios includes PROPERTIES PROCESSORS 8)
    set_tests_properties(deploy PROPERTIES PROCESSORS 4)
    set_tests_properties(cleanup remote PROPERTIES PROCESSORS 2)
  endif()
endif()
