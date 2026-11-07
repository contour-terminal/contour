# SPDX-License-Identifier: Apache-2.0
#
# Check for the contour_e2e_signed_overlay test: runs cmake/WindowsSignedOverlay.cmake, the CPACK_PRE_BUILD_SCRIPTS
# hook that lays the signed files over CPack's staging directory, against hand-made staging trees and signing
# manifests, and asserts what it replaces and what it refuses.
#
# Run by ctest as
#   cmake -D HOOK=<cmake/WindowsSignedOverlay.cmake> -D WORK_DIR=<scratch directory> -P SignedOverlay.cmake
#
# CPack itself is not involved, so no WiX and no package are needed. The hook is run in a child cmake, the way
# CPack runs it, and judged by its exit code and by the error text it prints, either of which marks a failure.
# Every path holds a space, because the install folder of a real build does.
# The verdict of this script is the marker printed last, which the test's PASS_REGULAR_EXPRESSION requires.

if(NOT EXISTS "${HOOK}")
    message(FATAL_ERROR "HOOK does not name an existing file: '${HOOK}'")
endif()
if(NOT WORK_DIR)
    message(FATAL_ERROR "WORK_DIR is not set")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Writes <path> with <content>, creating the directory.
function(write_file path content)
    get_filename_component(_dir "${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${path}" "${content}")
endfunction()

# Writes a manifest of rows, each "<relative>\t<signed path>\t<before>\t<after>", after a comment header.
function(write_manifest path)
    set(_text "# relative-path\tsigned-file\tsha256-before\tsha256-after\n")
    foreach(_row IN LISTS ARGN)
        string(APPEND _text "${_row}\n")
    endforeach()
    write_file("${path}" "${_text}")
endfunction()

# CMake wraps its error messages at a fixed width, inside long paths too, so the comparison ignores white space.
function(squeeze out text)
    string(REGEX REPLACE "[ \t\r\n]+" "" _squeezed "${text}")
    set(${out} "${_squeezed}" PARENT_SCOPE)
endfunction()

function(expect_contains what text needle)
    squeeze(text "${text}")
    squeeze(needle "${needle}")
    string(FIND "${text}" "${needle}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "${what}: expected to find '${needle}' in:\n${text}")
    endif()
endfunction()

function(expect_not_contains what text needle)
    squeeze(text "${text}")
    squeeze(needle "${needle}")
    string(FIND "${text}" "${needle}" _at)
    if(NOT _at EQUAL -1)
        message(FATAL_ERROR "${what}: did not expect '${needle}' in:\n${text}")
    endif()
endfunction()

function(expect_content what path expected)
    file(READ "${path}" _actual)
    if(NOT "${_actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: ${path} holds '${_actual}', expected '${expected}'")
    endif()
endfunction()

# Runs the hook for one staging tree; sets RAN_OUT (stdout and stderr, joined) and RAN_FAILED (whether the text
# says the hook stopped on an error). A manifest of "" runs the hook without CPACK_CONTOUR_SIGNING_MANIFEST.
function(run_hook staging generator manifest)
    set(_manifest_args "")
    if(NOT "${manifest}" STREQUAL "")
        set(_manifest_args -D "CPACK_CONTOUR_SIGNING_MANIFEST=${manifest}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND}
                -D "CPACK_TEMPORARY_INSTALL_DIRECTORY=${staging}"
                -D "CPACK_GENERATOR=${generator}"
                ${_manifest_args}
                -P "${HOOK}"
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc)
    set(_all "${_out}${_err}")
    set(RAN_OUT "${_all}" PARENT_SCOPE)
    string(FIND "${_all}" "CMake Error" _at)
    if(_at EQUAL -1 AND _rc EQUAL 0)
        set(RAN_FAILED FALSE PARENT_SCOPE)
    else()
        set(RAN_FAILED TRUE PARENT_SCOPE)
    endif()
endfunction()

set(_before_text "unsigned bytes")
set(_after_text "signed bytes")
string(SHA256 _before "${_before_text}")
string(SHA256 _after "${_after_text}")

# The signed copy that a manifest points at, outside the staging tree, as the signer step leaves it.
set(_signed "${WORK_DIR}/signed files/bin/contour.exe")
write_file("${_signed}" "${_after_text}")

# 1. Component layout: CPack stages <child>/bin/contour.exe, the manifest names bin/contour.exe.
set(_staging "${WORK_DIR}/case component/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_manifest("${WORK_DIR}/case component/manifest.tsv"
    "bin/contour.exe\t${_signed}\t${_before}\t${_after}")
run_hook("${_staging}" WIX "${WORK_DIR}/case component/manifest.tsv")
if(RAN_FAILED)
    message(FATAL_ERROR "component layout: the hook failed:\n${RAN_OUT}")
endif()
expect_content("component layout" "${_staging}/Unspecified/bin/contour.exe" "${_after_text}")
expect_contains("component layout" "${RAN_OUT}" "1 files from the signing manifest")

# 2. Flat layout (the ZIP generator without components): <staging>/bin/contour.exe.
set(_staging "${WORK_DIR}/case flat/tmp")
write_file("${_staging}/bin/contour.exe" "${_before_text}")
write_manifest("${WORK_DIR}/case flat/manifest.tsv"
    "bin/contour.exe\t${_signed}\t${_before}\t${_after}")
run_hook("${_staging}" ZIP "${WORK_DIR}/case flat/manifest.tsv")
if(RAN_FAILED)
    message(FATAL_ERROR "flat layout: the hook failed:\n${RAN_OUT}")
endif()
expect_content("flat layout" "${_staging}/bin/contour.exe" "${_after_text}")

# 3. Identity: a file the signer left as it was (Before == After) is accepted and untouched.
set(_staging "${WORK_DIR}/case identity/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_manifest("${WORK_DIR}/case identity/manifest.tsv"
    "bin/contour.exe\t${_signed}\t${_before}\t${_before}")
run_hook("${_staging}" WIX "${WORK_DIR}/case identity/manifest.tsv")
if(RAN_FAILED)
    message(FATAL_ERROR "identity: the hook failed:\n${RAN_OUT}")
endif()
expect_content("identity" "${_staging}/Unspecified/bin/contour.exe" "${_before_text}")

# 4. A file the manifest names that the staging tree does not hold is an error that names it.
set(_staging "${WORK_DIR}/case missing/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_manifest("${WORK_DIR}/case missing/manifest.tsv"
    "bin/missing.dll\t${_signed}\t${_before}\t${_after}")
run_hook("${_staging}" WIX "${WORK_DIR}/case missing/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "missing from staging: the hook accepted it:\n${RAN_OUT}")
endif()
expect_contains("missing from staging" "${RAN_OUT}" "bin/missing.dll")

# 5. A staged file that is neither the unsigned nor the signed bytes is not the file that was signed.
set(_staging "${WORK_DIR}/case stale/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "a rebuilt binary")
write_manifest("${WORK_DIR}/case stale/manifest.tsv"
    "bin/contour.exe\t${_signed}\t${_before}\t${_after}")
run_hook("${_staging}" WIX "${WORK_DIR}/case stale/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "stale staged file: the hook accepted it:\n${RAN_OUT}")
endif()
expect_contains("stale staged file" "${RAN_OUT}" "not the file that was signed")
expect_content("stale staged file" "${_staging}/Unspecified/bin/contour.exe" "a rebuilt binary")

# 6. No manifest variable: the hook does nothing, and says nothing.
set(_staging "${WORK_DIR}/case none/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
run_hook("${_staging}" WIX "")
if(RAN_FAILED)
    message(FATAL_ERROR "no manifest: the hook failed:\n${RAN_OUT}")
endif()
expect_not_contains("no manifest" "${RAN_OUT}" "WindowsSignedOverlay")
expect_content("no manifest" "${_staging}/Unspecified/bin/contour.exe" "${_before_text}")

# 7. A manifest path that does not exist is an error that names it.
run_hook("${WORK_DIR}/case none/tmp" WIX "${WORK_DIR}/case absent/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "absent manifest: the hook accepted it:\n${RAN_OUT}")
endif()
expect_contains("absent manifest" "${RAN_OUT}" "case absent/manifest.tsv")

# 8. A row with too few columns is an error that names the line.
set(_staging "${WORK_DIR}/case malformed/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_file("${WORK_DIR}/case malformed/manifest.tsv"
    "# relative-path\tsigned-file\tsha256-before\tsha256-after\nbin/contour.exe\t${_signed}\t${_before}\n")
run_hook("${_staging}" WIX "${WORK_DIR}/case malformed/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "malformed row: the hook accepted it:\n${RAN_OUT}")
endif()
expect_contains("malformed row" "${RAN_OUT}" "line 2")

# 9. A binary CPack staged that the manifest does not name would ship as it is: an error that names it.
set(_staging "${WORK_DIR}/case unnamed/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_file("${_staging}/Unspecified/bin/extra.dll" "a library the signed stage lacks")
write_manifest("${WORK_DIR}/case unnamed/manifest.tsv"
    "bin/contour.exe	${_signed}	${_before}	${_after}")
run_hook("${_staging}" WIX "${WORK_DIR}/case unnamed/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "unnamed binary: the hook accepted it:
${RAN_OUT}")
endif()
expect_contains("unnamed binary" "${RAN_OUT}" "bin/extra.dll")
expect_contains("unnamed binary" "${RAN_OUT}" "not in the signing manifest")

# 10. A file the signer left alone is named with "-", and must be the staged one.
string(SHA256 _kept "a vendor library")
set(_staging "${WORK_DIR}/case kept/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_file("${_staging}/Unspecified/bin/vendor.dll" "a vendor library")
write_manifest("${WORK_DIR}/case kept/manifest.tsv"
    "bin/contour.exe	${_signed}	${_before}	${_after}"
    "bin/vendor.dll	-	${_kept}	${_kept}")
run_hook("${_staging}" WIX "${WORK_DIR}/case kept/manifest.tsv")
if(RAN_FAILED)
    message(FATAL_ERROR "kept file: the hook failed:
${RAN_OUT}")
endif()
expect_contains("kept file" "${RAN_OUT}" "2 files from the signing manifest")
expect_content("kept file" "${_staging}/Unspecified/bin/vendor.dll" "a vendor library")
write_file("${_staging}/Unspecified/bin/vendor.dll" "another build of it")
run_hook("${_staging}" WIX "${WORK_DIR}/case kept/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "kept file changed: the hook accepted it:
${RAN_OUT}")
endif()
expect_contains("kept file changed" "${RAN_OUT}" "not the file that was signed")

# 11. A signed copy that is not the one the manifest promised (a signer run again, a result replaced by hand).
set(_other_signed "${WORK_DIR}/other signed/bin/contour.exe")
write_file("${_other_signed}" "bytes the manifest does not know")
set(_staging "${WORK_DIR}/case promised/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_manifest("${WORK_DIR}/case promised/manifest.tsv"
    "bin/contour.exe	${_other_signed}	${_before}	${_after}")
run_hook("${_staging}" WIX "${WORK_DIR}/case promised/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "signed copy that differs: the hook accepted it:
${RAN_OUT}")
endif()
expect_contains("signed copy that differs" "${RAN_OUT}" "does not have the sha256 the manifest promises")

# 12. A signed copy that is not there.
set(_staging "${WORK_DIR}/case gone/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_manifest("${WORK_DIR}/case gone/manifest.tsv"
    "bin/contour.exe	${WORK_DIR}/no such dir/contour.exe	${_before}	${_after}")
run_hook("${_staging}" WIX "${WORK_DIR}/case gone/manifest.tsv")
if(NOT RAN_FAILED)
    message(FATAL_ERROR "signed copy that is gone: the hook accepted it:
${RAN_OUT}")
endif()
expect_contains("signed copy that is gone" "${RAN_OUT}" "the signed copy of bin/contour.exe does not exist")

# 13. A path with a character outside ASCII, as the manifest (UTF-8) holds it.
set(_umlaut_signed "${WORK_DIR}/Über uns/bin/contour.exe")
write_file("${_umlaut_signed}" "${_after_text}")
set(_staging "${WORK_DIR}/case umlaut/tmp")
write_file("${_staging}/Unspecified/bin/contour.exe" "${_before_text}")
write_manifest("${WORK_DIR}/case umlaut/manifest.tsv"
    "bin/contour.exe	${_umlaut_signed}	${_before}	${_after}")
run_hook("${_staging}" WIX "${WORK_DIR}/case umlaut/manifest.tsv")
if(RAN_FAILED)
    message(FATAL_ERROR "non-ASCII path: the hook failed:
${RAN_OUT}")
endif()
expect_content("non-ASCII path" "${_staging}/Unspecified/bin/contour.exe" "${_after_text}")

message("signed-overlay: OK")
