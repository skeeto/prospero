# libunabashed, the from-scratch Bash 5.3 as a library, behind the
# "bash (builtin)" shell: `prospero --bash -c CMD` (bashhost). Pinned by
# its release tarball's URL and SHA256, or taken from deps/unabashed
# when present (FetchContentBundled.cmake).
#
#   unabashed  0.7.2 (UB_VERSION 0x000700)
#
# As a subproject it defines only the library: libunabashed, an object
# library (alias unabashed::unabashed) with unabashed.h on its include
# path, built with its own flags and the host's build type. Public
# domain.

include(FetchContent)
include(FetchContentBundled)

fetch_content_bundled(unabashed unabashed)
FetchContent_Declare(unabashed
  URL https://github.com/skeeto/unabashed/releases/download/v0.7.2/unabashed-0.7.2.tar.gz
  URL_HASH SHA256=f8af868e14a8822d0675bd7d3c6defa86292f44f2e76d27743690d6af0942283
)
FetchContent_MakeAvailable(unabashed)
