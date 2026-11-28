# libunabashed, the from-scratch Bash 5.3 as a library, behind the
# "bash (builtin)" shell: `prospero --bash -c CMD` (bashhost). Pinned by
# its release tarball's URL and SHA256, or taken from deps/unabashed
# when present (FetchContentBundled.cmake).
#
#   unabashed  0.8.0 (UB_VERSION 0x000800)
#
# As a subproject it defines only the library: libunabashed, an object
# library (alias unabashed::unabashed) with unabashed.h on its include
# path, built with its own flags and the host's build type. Public
# domain.

include(FetchContent)
include(FetchContentBundled)

fetch_content_bundled(unabashed unabashed)
FetchContent_Declare(unabashed
  URL https://github.com/skeeto/unabashed/releases/download/v0.8.0/unabashed-0.8.0.tar.gz
  URL_HASH SHA256=cc428d542360fc9a42f82a9182fa5c0f2ad54cfd8f6b55706636dcf1b8f749b3
)
FetchContent_MakeAvailable(unabashed)
