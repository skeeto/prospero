# ariel: terminal emulation in C11. Builds on its own or inside another
# project through add_subdirectory:
#
#   target: ariel, an OBJECT library of ariel.c (include ariel.h)
#   ARIEL_TESTS: tests/test.c as the ariel_unit test, and the inputs in
#                tests/fuzz/ replayed through the fuzz target's checks as
#                ariel_fuzz (top level: ON)
#   ARIEL_TOOLS: ariel-replay and the fuzz target (top level: ON)
#
# A parent may define ARIEL_TRAP on the target so assertions trap in
# release builds too.
cmake_minimum_required(VERSION 3.21)
project(ariel VERSION 0.1.0 LANGUAGES C)

option(ARIEL_TESTS "Build ariel's tests" ${PROJECT_IS_TOP_LEVEL})
option(ARIEL_TOOLS "Build ariel-replay and the fuzz target" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Debug builds trap on undefined behavior, failed assertions included,
# and optimize a little (-Og) to stay within a few times Release's speed.
set(ARIEL_WARNINGS "")
set(ARIEL_DEBUG "")
set(ARIEL_DEBUGOPT "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(ARIEL_WARNINGS -Wall -Wextra -Wconversion -Wno-sign-conversion -Wvla)
  set(ARIEL_DEBUGOPT -Og)
  include(CheckCCompilerFlag)
  check_c_compiler_flag(-fsanitize-trap ARIEL_HAS_TRAP)
  if(ARIEL_HAS_TRAP)
    set(ARIEL_DEBUG -fsanitize=undefined -fsanitize-trap)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
      list(APPEND ARIEL_DEBUG -fno-sanitize=alignment)
    endif()
  endif()
endif()

function(ariel_flags target)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE ${ARIEL_WARNINGS}
    "$<$<CONFIG:Debug>:${ARIEL_DEBUGOPT};${ARIEL_DEBUG}>")
  target_link_options(${target} PRIVATE "$<$<CONFIG:Debug>:${ARIEL_DEBUG}>")
endfunction()

add_library(ariel OBJECT ariel.c)
target_include_directories(ariel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
ariel_flags(ariel)

if(ARIEL_TESTS)
  enable_testing()
  add_executable(ariel_test tests/test.c)
  ariel_flags(ariel_test)
  # The harness itself uses stdio and needn't be as strict
  target_compile_options(ariel_test PRIVATE -Wno-conversion)
  file(GLOB ARIEL_SCRIPTS CONFIGURE_DEPENDS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vterm/*.test)
  file(GLOB ARIEL_RECORDINGS CONFIGURE_DEPENDS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rec/*.vt)
  add_test(NAME ariel_unit
    COMMAND ariel_test grapheme.txt emoji.txt ${ARIEL_SCRIPTS} ${ARIEL_RECORDINGS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  # Minimized fuzz finds, kept as regressions
  add_executable(ariel_fuzzcheck tests/fuzz.c)
  ariel_flags(ariel_fuzzcheck)
  add_test(NAME ariel_fuzz
    COMMAND ${CMAKE_COMMAND} -DFUZZ=$<TARGET_FILE:ariel_fuzzcheck>
            -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz.cmake)
endif()

if(ARIEL_TOOLS)
  add_executable(ariel-replay tools/replay.c)
  ariel_flags(ariel-replay)
  # forkpty, and POSIX declarations beside strict C11
  if(NOT WIN32)
    target_compile_definitions(ariel-replay PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)
    if(NOT APPLE)
      target_link_libraries(ariel-replay PRIVATE util)
    endif()
  endif()
  add_executable(ariel-fuzz tests/fuzz.c)
  ariel_flags(ariel-fuzz)
endif()
