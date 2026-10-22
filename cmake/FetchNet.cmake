# The POSIX HTTP stack: cpp-httplib (header-only) over mbedTLS, as the
# `httpnet` INTERFACE target. Windows uses WinHTTP instead, so this is
# included only on POSIX. Patterns from elfeed2's FetchTLS.cmake and
# FetchCppHttplib.cmake, without zlib: the harness never asks for
# compressed responses (WinHTTP doesn't either), and gzip would only
# delay streamed tokens.
#
# mbedTLS 3.6 is the LTS line. Its TLS 1.3 support needs PSA crypto,
# which the edge initializes itself with psa_crypto_init().

include(FetchContent)
include(FetchContentBundled)

fetch_content_bundled(mbedtls mbedtls)
set(ENABLE_PROGRAMS        OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING         OFF CACHE BOOL "" FORCE)
set(MBEDTLS_AS_SUBPROJECT  ON  CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
)
FetchContent_MakeAvailable(mbedtls)

# SOURCE_SUBDIR names a missing directory so that only the sources are
# populated: cpp-httplib's own CMakeLists would look for OpenSSL.
fetch_content_bundled(cpp-httplib cpp-httplib)
FetchContent_Declare(
  cpp-httplib
  URL           https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.43.0.tar.gz
  URL_HASH      SHA256=28b88a7f0751c762cc9c70c91db0d6953554c40f49285907e716dd7943a824a5
  SOURCE_SUBDIR _no_cmake_
)
FetchContent_MakeAvailable(cpp-httplib)

if(NOT TARGET httpnet)
  add_library(httpnet INTERFACE)
  target_include_directories(httpnet SYSTEM INTERFACE "${cpp-httplib_SOURCE_DIR}")
  target_link_libraries(httpnet INTERFACE mbedtls mbedx509 mbedcrypto)
  # On macOS, mbedTLS's roots come from /etc/ssl/cert.pem, Apple's
  # anchors alone, rather than the keychain, but the system decides on
  # each chain (http_httplib.cpp). Security and CoreFoundation are loaded
  # then, not linked: linking them costs every process start about a
  # millisecond, and every tool process starts under a reaper that is
  # this executable.
  target_compile_definitions(httpnet INTERFACE
    CPPHTTPLIB_MBEDTLS_SUPPORT CPPHTTPLIB_NO_EXCEPTIONS
    CPPHTTPLIB_DISABLE_MACOSX_AUTOMATIC_ROOT_CERTIFICATES)
endif()
