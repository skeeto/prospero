cmake_minimum_required(VERSION 3.25)
# The oldest macOS the build runs on, rather than the build machine's own
# (a cache variable that project() reads, so it is set before it)
if(NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "Oldest macOS to run on")
endif()
project(prospero VERSION 0.8.3 LANGUAGES C CXX)

# Unity build: each executable is one translation unit, a platform layer
# (src/main_*.cpp) that includes the portable core (src/prospero.cpp). The
# core has no C++ runtime library, exceptions, or RTTI. Third-party code
# at the edges (GUI, HTTP) arrives in later milestones with its own flags.

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(PROSPERO_NAME prospero CACHE STRING "Executable name")
option(PROSPERO_GUI "Build the GUI (OFF: the headless engine only)" ON)
option(PROSPERO_DEVTOOLS "The GUI's developer tools, the Metrics pane (always in Debug builds)" OFF)
option(PROSPERO_STATIC "Link fully statically (Linux: deployed engines)" OFF)
# The edition: "community", what a source build and the GitHub releases
# are, or "itch", the itch.io builds (tools/itch.py), named plain
# "Prospero", with the engines zip (tools/engines.py) embedded, so that
# deploy needs no engines directory
set(PROSPERO_EDITION community CACHE STRING "Edition: community or itch")
set_property(CACHE PROSPERO_EDITION PROPERTY STRINGS community itch)
set(PROSPERO_ENGINES_ZIP "" CACHE FILEPATH
    "For the itch edition: prospero-engines-VERSION.zip, embedded")
if(PROSPERO_EDITION STREQUAL "community")
  set(prospero_display_name "Prospero Community Edition")
elseif(PROSPERO_EDITION STREQUAL "itch")
  set(prospero_display_name "Prospero")
  if(NOT PROSPERO_GUI)
    message(FATAL_ERROR "the itch edition is a GUI build")
  endif()
  if(NOT IS_ABSOLUTE "${PROSPERO_ENGINES_ZIP}" OR NOT EXISTS "${PROSPERO_ENGINES_ZIP}")
    message(FATAL_ERROR "the itch edition needs PROSPERO_ENGINES_ZIP, the "
                        "engines zip's absolute path")
  endif()
else()
  message(FATAL_ERROR "PROSPERO_EDITION is community or itch, not ${PROSPERO_EDITION}")
endif()

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

# Debug builds also run under AddressSanitizer where the toolchain has
# it, which arenacheck.cpp tells of arena memory not allocated: fresh
# space, a copy's once it ends, and what a restore gives back. Not on
# Windows (no ASan in mingw) nor in a static build.
option(PROSPERO_ASAN "AddressSanitizer in Debug builds, where available" ON)
set(PROSPERO_ASANFLAGS "")
if(PROSPERO_ASAN AND NOT WIN32 AND NOT PROSPERO_STATIC)
  check_linker_flag(CXX -fsanitize=address PROSPERO_HAS_ASAN)
  check_cxx_compiler_flag(-fsanitize-trap=undefined PROSPERO_HAS_TRAPUB)
  if(PROSPERO_HAS_ASAN AND (PROSPERO_HAS_TRAPUB OR NOT PROSPERO_HAS_TRAP))
    set(PROSPERO_ASANFLAGS -fsanitize=address -fno-omit-frame-pointer)
    if(PROSPERO_HAS_TRAP)
      # Undefined behavior still traps, but ASan reports
      list(TRANSFORM PROSPERO_TRAP REPLACE "^-fsanitize-trap$" "-fsanitize-trap=undefined")
    endif()
  endif()
endif()
target_compile_options(prospero_core_flags INTERFACE
  "$<$<CONFIG:Debug>:-g3;-Og;${PROSPERO_TRAP};${PROSPERO_ASANFLAGS}>")
target_link_options(prospero_core_flags INTERFACE
  "$<$<CONFIG:Debug>:${PROSPERO_TRAP};${PROSPERO_ASANFLAGS}>")

# Debug builds check arenas for aliasing (src/arenacheck.cpp), as the
# unit tests always do: an arena allocating again over bytes another
# allocated over meanwhile, or allocating once destroyed.
option(PROSPERO_ARENACHECK "Check arenas for aliasing in Debug builds" ON)
if(PROSPERO_ARENACHECK)
  target_compile_definitions(prospero_core_flags INTERFACE
    "$<$<CONFIG:Debug>:PROSPERO_ARENACHECK=1>")
endif()

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

# The core embeds the seed configuration (config.cpp), the Help pages
# and the built-in skill (help.cpp) with #embed, and the GUI its fonts
# (fonts.cpp), the Community Edition's greeting (ui.cpp) and, on X11
# and Wayland, its window icon (gui_sdl.cpp), which not every compiler
# lists as dependencies.
file(GLOB prospero_help CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/assets/help/*.md)
set_property(SOURCE src/main_posix.cpp src/main_win32.cpp src/main_test.cpp
  APPEND PROPERTY OBJECT_DEPENDS ${prospero_help}
  ${PROJECT_SOURCE_DIR}/assets/skills/prospero/SKILL.md
  ${PROJECT_SOURCE_DIR}/assets/greeting.md
  ${PROJECT_SOURCE_DIR}/assets/prospero.ini
  ${PROJECT_SOURCE_DIR}/assets/Roboto-Medium.ttf
  ${PROJECT_SOURCE_DIR}/assets/JetBrainsMono-Regular.ttf
  ${PROJECT_SOURCE_DIR}/assets/NotoSansSymbols2-subset.ttf
  ${PROJECT_SOURCE_DIR}/assets/NotoSansMath-subset.ttf
  ${PROJECT_SOURCE_DIR}/assets/codicon.ttf
  ${PROJECT_SOURCE_DIR}/assets/prospero.png)

# Every program with the core decodes images (src/wuffs.c)
include(FetchWuffs)

if(PROSPERO_GUI)
  include(FetchGui)
endif()

if(WIN32 AND PROSPERO_GUI)
  # The GUI build has CRT startup, since FreeType, md4c and ImGui need a
  # CRT, though the core still never calls it. windows.h, D3D11 and the
  # ImGui Win32 backends stay in the gui_win32.cpp edge, which embeds
  # the icon and the manifest (assets/, through -Wa,-I).
  add_library(prospero_gui OBJECT src/gui_win32.cpp)
  set_property(SOURCE src/gui_win32.cpp APPEND PROPERTY OBJECT_DEPENDS
    ${PROJECT_SOURCE_DIR}/assets/prospero.ico
    ${PROJECT_SOURCE_DIR}/assets/prospero.manifest)
  target_link_libraries(prospero_gui PRIVATE imgui)
  target_compile_definitions(prospero_gui PRIVATE PROSPERO_GUIEDGE=1)
  target_compile_options(prospero_gui PRIVATE
    -fno-exceptions -fno-rtti -Wa,-I${PROJECT_SOURCE_DIR}
  )
  # The desktop helper is a command of this program, "prospero desktop",
  # in a translation unit of its own (src/deskwin32.cpp)
  add_executable(prospero WIN32 src/main_win32.cpp src/deskwin32.cpp)
  target_link_libraries(prospero PRIVATE
    prospero_gui imgui md4c kernel32 advapi32 winhttp comdlg32 ole32
    user32 gdi32
  )
  target_link_options(prospero PRIVATE -static -municode)
elseif(WIN32)
  # CRT-free: system DLLs only. Static libgcc keeps its helpers (e.g.
  # 64-bit division on i686) from adding a DLL dependency. Nothing is
  # auto-imported: under LTO, ld would otherwise bring in mingw's
  # pseudo-relocation runtime, and with it msvcrt.dll.
  add_executable(prospero src/main_win32.cpp src/deskwin32.cpp)
  target_link_options(prospero PRIVATE -nostartfiles -static-libgcc
    -Wl,--disable-auto-import)
  target_link_libraries(prospero PRIVATE kernel32 advapi32 winhttp user32 gdi32)
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
target_link_libraries(prospero PRIVATE prospero_core_flags wuffs)
if(WIN32)
  # A zero PE timestamp, as w64devkit's ld writes by default, rather than
  # the link time, so the same sources link the same program with any
  # GNU toolchain. Releases check it (tools/pecheck.py).
  target_link_options(prospero PRIVATE -Wl,--no-insert-timestamp)
endif()
if(PROSPERO_EDITION STREQUAL "itch")
  target_compile_definitions(prospero PRIVATE PROSPERO_ITCH=1
    "PROSPERO_ENGINES_ZIP=\"${PROSPERO_ENGINES_ZIP}\"")
endif()
if(PROSPERO_GUI)
  target_compile_definitions(prospero PRIVATE PROSPERO_GUI=1)
  target_compile_definitions(prospero PRIVATE
    $<$<OR:$<BOOL:${PROSPERO_DEVTOOLS}>,$<CONFIG:Debug>>:PROSPERO_DEVTOOLS=1>)
endif()
if(APPLE)
  # A code-signing identity from the keychain (its name, as listed by
  # "security find-identity -p codesigning"). Privacy grants such as
  # Screen Recording and Accessibility belong to a signature, and the
  # linker's ad-hoc signature changes with every build, so a stable
  # identity, even a self-signed one, keeps them across rebuilds.
  set(PROSPERO_SIGN_IDENTITY "" CACHE STRING
      "macOS code-signing identity for the built programs (empty: ad hoc)")
  # The bundle identifier, which privacy grants name too, so that a test
  # build, signed ad hoc, never claims the real app's grants, nor leaves
  # a code requirement of its own on them that then refuses the real
  # app: com.nullprogram.prospero only for a signed build, by default.
  # The desktop helper's is this plus ".desktop".
  set(PROSPERO_BUNDLE_ID "" CACHE STRING
      "macOS bundle identifier (empty: com.nullprogram.prospero when signed, else .dev)")
  if(PROSPERO_BUNDLE_ID)
    set(prospero_bundle_id ${PROSPERO_BUNDLE_ID})
  elseif(PROSPERO_SIGN_IDENTITY)
    set(prospero_bundle_id com.nullprogram.prospero)
  else()
    set(prospero_bundle_id com.nullprogram.prospero.dev)
  endif()
  set(prospero_desktop_id ${prospero_bundle_id}.desktop)
  if(NOT PROSPERO_SIGN_IDENTITY AND prospero_bundle_id STREQUAL "com.nullprogram.prospero")
    message(WARNING "an ad-hoc build with the real app's bundle identifier "
                    "can claim, and spoil, its privacy grants")
  endif()
  # Ad hoc, unless an identity is given: either way the identifiers
  # are ours rather than ones made from file names
  if(PROSPERO_SIGN_IDENTITY)
    set(prospero_sign ${PROSPERO_SIGN_IDENTITY})
  else()
    set(prospero_sign -)
  endif()
  # A Developer ID signs for distribution, which notarization requires
  # to have the hardened runtime and a secure timestamp. Others sign for
  # this machine alone, with no round trip to Apple's timestamp server.
  if(PROSPERO_SIGN_IDENTITY MATCHES "^Developer ID Application")
    set(prospero_signopts --options runtime --timestamp)
  else()
    set(prospero_signopts --timestamp=none)
  endif()
endif()
if(APPLE AND PROSPERO_SIGN_IDENTITY)
  add_custom_command(TARGET prospero POST_BUILD
    COMMAND codesign --force ${prospero_signopts}
            --sign ${PROSPERO_SIGN_IDENTITY} $<TARGET_FILE:prospero>
    VERBATIM
  )
endif()
set_target_properties(prospero PROPERTIES OUTPUT_NAME ${PROSPERO_NAME})

if(APPLE)
  # The desktop helper (src/deskhelp.cpp), which the desktop tool runs
  # from next to the harness: CoreGraphics, Accessibility and the
  # Objective-C runtime's C functions, with ScreenCaptureKit and AppKit
  # loaded at run time. Signed like the harness, since the privacy
  # grants it needs belong to a signature.
  set(prospero_desktop_libs prospero_core_flags
      "-framework ApplicationServices" "-framework Carbon" objc)
  add_executable(prospero-desktop src/main_desktop_macos.cpp)
  target_link_libraries(prospero-desktop PRIVATE ${prospero_desktop_libs})
  set_target_properties(prospero-desktop PROPERTIES
    OUTPUT_NAME ${PROSPERO_NAME}-desktop)
  # A fixed identifier too, which the grants name when it runs as its
  # own responsible process, rather than one made from its file name
  add_custom_command(TARGET prospero-desktop POST_BUILD
    COMMAND codesign --force ${prospero_signopts}
            --identifier ${prospero_desktop_id}
            --sign ${prospero_sign} $<TARGET_FILE:prospero-desktop>
    VERBATIM
  )
endif()

if(APPLE AND PROSPERO_GUI)
  # The same binary, also wrapped as an application bundle, which macOS
  # wants for a window of its own in the Dock and app switcher, with the
  # desktop helper beside it, where the harness looks. The bundle is
  # put together, and signed, after both are built, since its signature
  # seals the helper in it, ad hoc unless PROSPERO_SIGN_IDENTITY. Its
  # Info.plist names the bundle identifier and the version, and its
  # build number (CFBundleVersion) is the time of the build, in seconds:
  # LaunchServices prefers the highest, so that "open -a Prospero" runs
  # the newest of the copies it knows, as in old worktrees. The icon is
  # assets/prospero.icns (tools/icons.py).
  configure_file(assets/Info.plist ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
  add_custom_command(OUTPUT Prospero.app/Contents/MacOS/prospero
    COMMAND ${CMAKE_COMMAND} -E make_directory Prospero.app/Contents/MacOS
            Prospero.app/Contents/Resources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_BINARY_DIR}/Info.plist Prospero.app/Contents/Info.plist
    COMMAND sh -c "plutil -replace CFBundleVersion -string \"$(date +%s)\" Prospero.app/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/assets/prospero.icns
            Prospero.app/Contents/Resources/prospero.icns
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:prospero-desktop> Prospero.app/Contents/MacOS/prospero-desktop
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:prospero> Prospero.app/Contents/MacOS/prospero
    COMMAND codesign --force ${prospero_signopts} --sign ${prospero_sign} Prospero.app
    DEPENDS prospero prospero-desktop ${CMAKE_BINARY_DIR}/Info.plist
            ${PROJECT_SOURCE_DIR}/assets/prospero.icns
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
  )
  add_custom_target(prospero-app ALL
    DEPENDS ${CMAKE_BINARY_DIR}/Prospero.app/Contents/MacOS/prospero)

  # The bundle, signed as built, is what installs, and "cmake --build
  # build --target package" puts it on a disk image beside a link to
  # /Applications. With a Developer ID identity, cmake/notarize.cmake
  # then signs the image too and, given PROSPERO_NOTARY_PROFILE (a
  # profile from "xcrun notarytool store-credentials") or an App Store
  # Connect API key in the environment, notarizes and staples it. The
  # dependencies have install rules of their own, for their headers and
  # libraries, so it holds only the component "prospero".
  install(DIRECTORY ${CMAKE_BINARY_DIR}/Prospero.app DESTINATION .
          USE_SOURCE_PERMISSIONS COMPONENT prospero)
  set(PROSPERO_NOTARY_PROFILE "" CACHE STRING
      "notarytool keychain profile for notarizing the package (empty: none)")
  set(CPACK_GENERATOR DragNDrop)
  set(CPACK_PACKAGE_NAME Prospero)
  set(CPACK_PACKAGE_FILE_NAME
      prospero-${PROJECT_VERSION}-macos-${CMAKE_SYSTEM_PROCESSOR})
  set(CPACK_DMG_VOLUME_NAME Prospero)
  set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};prospero;prospero;/")
  set(CPACK_PROSPERO_SIGN_IDENTITY ${PROSPERO_SIGN_IDENTITY})
  set(CPACK_PROSPERO_NOTARY_PROFILE ${PROSPERO_NOTARY_PROFILE})
  set(CPACK_POST_BUILD_SCRIPTS ${PROJECT_SOURCE_DIR}/cmake/notarize.cmake)
  include(CPack)
endif()

# On Windows the desktop helper is prospero.exe itself (above), and so
# on Linux in a build that is not static: there it is "prospero desktop"
# too, the same source as a translation unit of its own. A static
# engine cannot dlopen, so it gets the helper as a program of its own.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT PROSPERO_STATIC)
  target_sources(prospero PRIVATE src/main_desktop_x11.cpp)
  target_link_libraries(prospero PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(prospero PRIVATE PROSPERO_DESKTOP=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # On X11 and Wayland (src/main_desktop_x11.cpp, src/deskwayland.cpp):
  # libX11, libXext and libXtst, and libei, libxkbcommon and libpipewire,
  # are loaded at run time, as SDL3 loads them, so it builds without
  # them, and never statically, which dlopen needs
  add_executable(prospero-desktop src/main_desktop_x11.cpp)
  target_link_libraries(prospero-desktop PRIVATE prospero_core_flags
    ${CMAKE_DL_LIBS})
  if(PROSPERO_HAS_NOSTDLIBXX)
    target_link_options(prospero-desktop PRIVATE -nostdlib++)
  endif()
  set_target_properties(prospero-desktop PROPERTIES
    OUTPUT_NAME ${PROSPERO_NAME}-desktop)
endif()
# Elsewhere the programs install as they are, to bin (as the component
# "prospero", apart from the dependencies' headers and libraries)
if(NOT (APPLE AND PROSPERO_GUI))
  install(TARGETS prospero COMPONENT prospero)
  if(TARGET prospero-desktop)
    install(TARGETS prospero-desktop COMPONENT prospero)
  endif()
endif()

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
  target_link_libraries(prospero_test PRIVATE prospero_core_flags wuffs)
  target_compile_definitions(prospero_test PRIVATE PROSPERO_ARENACHECK=1)
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
    # The macOS desktop helper's own layer, which never touches the
    # screen: its pure parts, and its commands without permissions
    add_executable(prospero-desktop-test src/main_desktop_macos.cpp)
    target_compile_definitions(prospero-desktop-test PRIVATE DK_SELFTEST=1)
    target_link_libraries(prospero-desktop-test PRIVATE ${prospero_desktop_libs})
    add_test(NAME desktop_unit COMMAND prospero-desktop-test)
    if(UV AND PROSPERO_GUI)
      # The bundle's identifiers, never the real app's in a test build
      set(prospero_signed)
      if(PROSPERO_SIGN_IDENTITY)
        set(prospero_signed --signed)
      endif()
      add_test(NAME bundle COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/bundle.py
        --app ${CMAKE_BINARY_DIR}/Prospero.app --helper $<TARGET_FILE:prospero-desktop>
        --version ${PROJECT_VERSION} ${prospero_signed}
        --bundle-id=${PROSPERO_BUNDLE_ID})
    endif()
    if(UV)
      # The helper itself, its commands and frames, permissions denied
      add_test(NAME desktop COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop_macos.py
        --helper $<TARGET_FILE:prospero-desktop>)
    endif()
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
    # The fake ssh, scp and desktop helper are Python, which Windows runs
    # as a program only through a shim (tests/shim_win32.c)
    set(prospero_shim)
    if(WIN32)
      add_executable(prospero_testshim tests/shim_win32.c)
      set(prospero_shim --shim $<TARGET_FILE:prospero_testshim>)
    endif()
    # Hosts and their clients on this machine (the ssh ones take --ssh)
    add_test(NAME remote COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/remote.py --prospero $<TARGET_FILE:prospero>
      ${prospero_shim})
    # URL includes in the configuration, against a fake web server
    add_test(NAME includes COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/includes.py --prospero $<TARGET_FILE:prospero>)
    # Deploying engines through a fake ssh and scp (a real host: --ssh)
    add_test(NAME deploy COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/deploy.py --prospero $<TARGET_FILE:prospero>
      ${prospero_shim})
    # The release's check of Authenticode signatures (tools/pecheck.py),
    # on a program signed with a throwaway key
    find_program(OPENSSL openssl)
    if(OPENSSL)
      add_test(NAME signcheck COMMAND ${UV} run
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/signcheck.py)
    endif()
    # The sources, for one arena given to a call twice: as the place for
    # its results and, by value, as its scratch
    add_test(NAME arenalint COMMAND ${UV} run
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/arenalint.py)
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
