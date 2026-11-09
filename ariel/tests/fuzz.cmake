# Replays each input in DIR through the fuzz target FUZZ, one input per
# run on its standard input, as without AFL
file(GLOB inputs ${DIR}/*)
set(failed 0)
foreach(f ${inputs})
  execute_process(COMMAND ${FUZZ} INPUT_FILE ${f} RESULT_VARIABLE r
                  OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT r EQUAL 0)
    message("${f}: ${r} ${err}")
    math(EXPR failed "${failed} + 1")
  endif()
endforeach()
list(LENGTH inputs n)
message("${n} inputs, ${failed} failed")
if(failed)
  message(FATAL_ERROR "fuzz inputs failed")
endif()
