# Helper that lets every FetchContent_Declare in the top-level
# CMakeLists fall back to a local checkout under deps/<dir> when
# present, instead of downloading and re-extracting on each clean
# build. Useful when developing with a hacked dep, or when offline.
#
# Usage:
#   include(FetchContentBundled)
#   fetch_content_bundled(imgui imgui)   # name, deps subdir
#   FetchContent_Declare(imgui URL ... URL_HASH ...)
#   FetchContent_MakeAvailable(imgui)
#
# The convention FETCHCONTENT_SOURCE_DIR_<NAME> is the documented
# way to override fetch behavior; we just compute the right name
# and point it at the right place.

#
# FetchContent caches the directory it was given, so once deps/<dir>
# is removed the cache still names it and configuring fails. Forget it
# then, but only when it is that deps/ path: a -D override stays.

function(fetch_content_bundled name dir)
  string(TOUPPER "${name}" upper)
  set(bundled "${CMAKE_SOURCE_DIR}/deps/${dir}")
  if(IS_DIRECTORY "${bundled}")
    set(FETCHCONTENT_SOURCE_DIR_${upper} "${bundled}" PARENT_SCOPE)
    message(STATUS "${name}: bundled (deps/${dir})")
  else()
    if("$CACHE{FETCHCONTENT_SOURCE_DIR_${upper}}" STREQUAL "${bundled}")
      unset(FETCHCONTENT_SOURCE_DIR_${upper} CACHE)
    endif()
    message(STATUS "${name}: downloaded")
  endif()
endfunction()
