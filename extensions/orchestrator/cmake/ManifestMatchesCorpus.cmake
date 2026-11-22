# SPDX-License-Identifier: Apache-2.0
#
# The test orchestrator_manifest_matches_corpus, run with `cmake -P`: runs MANIFEST_TOOL, writes what it prints
# to ACTUAL, and fails unless that is byte for byte the file EXPECTED.
foreach(_required IN ITEMS MANIFEST_TOOL EXPECTED ACTUAL)
    if(NOT ${_required})
        message(FATAL_ERROR "ManifestMatchesCorpus.cmake: ${_required} is not set")
    endif()
endforeach()

execute_process(COMMAND "${MANIFEST_TOOL}" OUTPUT_FILE "${ACTUAL}" RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${MANIFEST_TOOL} failed: ${_result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${ACTUAL}" "${EXPECTED}" RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    message(FATAL_ERROR "The generated manifest ${ACTUAL} differs from ${EXPECTED}: regenerate the corpus file "
                        "from the declared tables, in the same commit as the change to them.")
endif()
