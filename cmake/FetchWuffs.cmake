# Wuffs, Google's memory-safe image decoders, as its single-file C
# release, pinned by URL and SHA256, or taken from deps/wuffs (holding
# wuffs-v0.4.c) when present (FetchContentBundled.cmake).
#
#   wuffs  v0.4.0-alpha.12 (release/c/wuffs-v0.4.c at that tag)
#
# v0.3, the last release without a pre-release label, has no JPEG or
# WebP decoder. Target: wuffs, an object library of src/wuffs.c, which
# compiles the PNG, JPEG, GIF, BMP and WebP decoders (lossless and
# lossy) as C, with Wuffs' own flags, for every program that has the
# core. Licensed Apache-2.0 OR MIT.

include(FetchContent)
include(FetchContentBundled)

fetch_content_bundled(wuffs wuffs)
FetchContent_Declare(wuffs
  URL https://raw.githubusercontent.com/google/wuffs/v0.4.0-alpha.12/release/c/wuffs-v0.4.c
  URL_HASH SHA256=458066457347bd6d9bc3c906aa8292bea4488577727569ddd9436fdd89d5f04d
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(wuffs)

# Optimized in every build type: decoding at -O0 is many times slower,
# and the Debug build's checks belong to the core
add_library(wuffs OBJECT ${PROJECT_SOURCE_DIR}/src/wuffs.c)
target_include_directories(wuffs PRIVATE ${wuffs_SOURCE_DIR})
target_compile_options(wuffs PRIVATE -O2)
