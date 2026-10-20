# GUI dependencies, each pinned by URL and SHA256, or taken from a local
# checkout under deps/<dir> when present (FetchContentBundled.cmake).
#
#   imgui     v1.93.0-docking, by commit tarball
#   md4c      v0.6.0
#   freetype  2.15.0
#   SDL3      3.4.18 (macOS and Linux only)
#
# Targets: md4c, freetype, imgui (with its platform backends and the
# FreeType loader), and SDL3::SDL3-static on POSIX. Third-party code
# keeps its own flags; consumers see its headers as system headers, so
# the core's warnings stay quiet about them.

include(FetchContent)
include(FetchContentBundled)
set(FETCHCONTENT_QUIET ON)

fetch_content_bundled(imgui imgui)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/b916186e66aa3c02b72c6ca17455516512ed8049.tar.gz
  URL_HASH SHA256=a8e9ed7adb544e2d528722e96a229272716fb9e220a4b9e2ceee5469d548f562
)

fetch_content_bundled(md4c md4c)
FetchContent_Declare(md4c
  URL https://github.com/mity/md4c/archive/refs/tags/v0.6.0.tar.gz
  URL_HASH SHA256=4d151298125a81da3b2efa2e0eed8bdb7a9318569804e4fa4d7a2375ab83ef70
  SOURCE_SUBDIR _none  # only md4c.c is wanted, not its CMake project
)

fetch_content_bundled(freetype freetype)
FetchContent_Declare(freetype
  URL https://download.savannah.gnu.org/releases/freetype/freetype-2.15.0.tar.xz
  URL_HASH SHA256=93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5
  SYSTEM
)

FetchContent_MakeAvailable(imgui md4c)

add_library(md4c STATIC ${md4c_SOURCE_DIR}/src/md4c.c)
target_include_directories(md4c SYSTEM PUBLIC ${md4c_SOURCE_DIR}/src)
target_compile_definitions(md4c PUBLIC MD4C_USE_UTF8)

# FreeType without optional codecs: the fonts are embedded TrueType.
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HVF      ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVE ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(freetype)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVE})

if(NOT WIN32)
  fetch_content_bundled(SDL3 sdl3)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
    SYSTEM
  )
  # Video and events only. X11, Wayland and GL stay dlopened, so one
  # Linux binary still runs headless where no display libraries exist.
  set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
  set(SDL_AUDIO         OFF CACHE BOOL "" FORCE)
  set(SDL_GPU           OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER        OFF CACHE BOOL "" FORCE)
  set(SDL_CAMERA        OFF CACHE BOOL "" FORCE)
  set(SDL_JOYSTICK      OFF CACHE BOOL "" FORCE)
  set(SDL_HAPTIC        OFF CACHE BOOL "" FORCE)
  set(SDL_HIDAPI        OFF CACHE BOOL "" FORCE)
  set(SDL_POWER         OFF CACHE BOOL "" FORCE)
  set(SDL_SENSOR        OFF CACHE BOOL "" FORCE)
  set(SDL_TRAY          OFF CACHE BOOL "" FORCE)
  set(SDL_VULKAN        OFF CACHE BOOL "" FORCE)
  set(SDL_KMSDRM        OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

set(IMGUI ${imgui_SOURCE_DIR})
add_library(imgui STATIC
  ${IMGUI}/imgui.cpp
  ${IMGUI}/imgui_demo.cpp
  ${IMGUI}/imgui_draw.cpp
  ${IMGUI}/imgui_tables.cpp
  ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/misc/freetype/imgui_freetype.cpp
)
target_include_directories(imgui SYSTEM PUBLIC ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(imgui PUBLIC
  IMGUI_USER_CONFIG="${PROJECT_SOURCE_DIR}/src/imconfig.h"
)
target_compile_options(imgui PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>
)
target_link_libraries(imgui PUBLIC freetype)
if(WIN32)
  target_sources(imgui PRIVATE
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp
  )
  target_link_libraries(imgui PUBLIC
    d3d11 dxgi d3dcompiler dwmapi imm32 user32 gdi32 shell32
  )
else()
  target_sources(imgui PRIVATE
    ${IMGUI}/backends/imgui_impl_sdl3.cpp
    ${IMGUI}/backends/imgui_impl_opengl3.cpp
  )
  target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
endif()
