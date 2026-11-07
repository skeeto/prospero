# Run at each build (cmake -P): what source tree the build is of, for
# --version and About, as git describe names it, into OUT as
#
#   #define PROSPERO_DESCRIBE " (v0.15.0-9-ga38cb70-dirty)"
#
# or "" for a release: a clean checkout of tag vVERSION, or a tree with
# neither git nor a .describe. A tree with no .git takes the line
# tools/dist.py writes into .describe, for a tarball of an untagged
# commit. OUT is written only when it changes, so nothing is compiled
# again until the commit does.
#
#   cmake -DSRC=DIR -DOUT=FILE -DVERSION=X.Y.Z -P GitDescribe.cmake

set(desc "")
find_program(GIT git)
if(GIT AND EXISTS "${SRC}/.git")
  execute_process(
    COMMAND "${GIT}" -C "${SRC}" describe --tags --match "v*" --always --dirty
    OUTPUT_VARIABLE desc OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    set(desc "")
  endif()
elseif(EXISTS "${SRC}/.describe")
  file(STRINGS "${SRC}/.describe" desc LIMIT_COUNT 1)
endif()

# Only what a tag name and a hash hold, so it is a plain C string
string(REGEX REPLACE "[^A-Za-z0-9._+-]" "" desc "${desc}")
if(desc STREQUAL "v${VERSION}")
  set(desc "")
endif()
if(desc STREQUAL "")
  set(text "#define PROSPERO_DESCRIBE \"\"\n")
else()
  set(text "#define PROSPERO_DESCRIBE \" (${desc})\"\n")
endif()

set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
