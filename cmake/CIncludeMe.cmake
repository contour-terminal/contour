add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/cincludeme)

function(CIncludeMe INPUT_FILE OUTPUT_FILE SYMBOL_NAME NAMESPACE)
	add_custom_command(
		OUTPUT "${OUTPUT_FILE}"
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		COMMAND cincludeme "${INPUT_FILE}" "${OUTPUT_FILE}" "${SYMBOL_NAME}" "${NAMESPACE}"
		DEPENDS cincludeme
		COMMENT "Generating ${OUTPUT_FILE} from ${INPUT_FILE}"
		VERBATIM
	)
	set_source_files_properties("${OUTPUT_FILE}" PROPERTIES GENERATED TRUE)
endfunction()
