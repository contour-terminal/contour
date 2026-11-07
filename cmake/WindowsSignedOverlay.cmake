# SPDX-License-Identifier: Apache-2.0
#
# CPACK_PRE_BUILD_SCRIPTS hook: lay the signed Windows files over CPack's staging directory before CPack
# hands it to WiX or to the zip writer.
#
# Why here: CPack does not package the build tree, it re-runs the install rules into its own staging
# directory, so the files signed in a staged copy of the install (scripts/sign-windows.ps1 stage + sign) would
# never reach a package. This hook closes that gap: it reads the signing manifest, finds each named file in
# the staging directory and replaces it with the signed copy, checking SHA-256 digests on both sides so a
# stale or foreign file can never be shipped as "signed".
#
# CPACK_CONTOUR_SIGNING_MANIFEST (absolute path, passed by the Windows CI job as
# `cpack -D CPACK_CONTOUR_SIGNING_MANIFEST=...`) names the manifest. Unset, the hook does nothing, so a
# local `cpack` builds the unsigned packages exactly as before. The manifest format is documented in
# scripts/windows-signing/WindowsSigning.psm1 (Write-SigningManifest); the process is described in
# docs/internals/release-signing.md.
#
# Variables come from CPack (CPACK_TEMPORARY_INSTALL_DIRECTORY, CPACK_GENERATOR).

if(NOT CPACK_CONTOUR_SIGNING_MANIFEST)
    return()
endif()

if(NOT EXISTS "${CPACK_CONTOUR_SIGNING_MANIFEST}")
    message(FATAL_ERROR
        "WindowsSignedOverlay: the signing manifest does not exist: ${CPACK_CONTOUR_SIGNING_MANIFEST}")
endif()

# The staging layout depends on the generator: with install components (WIX) every file sits one directory
# deeper, as <staging>/<component>/<relative>, without them (ZIP) it sits at <staging>/<relative>. Both are
# probed, because which one CPack picks is an implementation detail and not worth breaking a release over.
set(_children "")
file(GLOB _entries LIST_DIRECTORIES true "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*")
foreach(_entry IN LISTS _entries)
    if(IS_DIRECTORY "${_entry}")
        list(APPEND _children "${_entry}")
    endif()
endforeach()

# The manifest is UTF-8 (without a byte order mark), and so are the paths in it.
file(STRINGS "${CPACK_CONTOUR_SIGNING_MANIFEST}" _lines ENCODING UTF-8)
set(_count 0)
set(_covered "")
set(_line_number 0)
foreach(_line IN LISTS _lines)
    math(EXPR _line_number "${_line_number} + 1")
    if(_line MATCHES "^#" OR _line STREQUAL "")
        continue()
    endif()
    string(REPLACE "\t" ";" _columns "${_line}")
    list(LENGTH _columns _column_count)
    if(NOT _column_count EQUAL 4)
        message(FATAL_ERROR
            "WindowsSignedOverlay: ${CPACK_CONTOUR_SIGNING_MANIFEST} line ${_line_number} has "
            "${_column_count} columns, expected 4 (relative path, signed file, sha256 before, sha256 after)")
    endif()
    list(GET _columns 0 _relative)
    list(GET _columns 1 _signed)
    list(GET _columns 2 _before)
    list(GET _columns 3 _after)

    set(_candidates "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/${_relative}")
    foreach(_child IN LISTS _children)
        list(APPEND _candidates "${_child}/${_relative}")
    endforeach()

    set(_found 0)
    foreach(_candidate IN LISTS _candidates)
        if(NOT EXISTS "${_candidate}" OR IS_DIRECTORY "${_candidate}")
            continue()
        endif()
        math(EXPR _found "${_found} + 1")
        list(APPEND _covered "${_candidate}")
        file(SHA256 "${_candidate}" _staged)
        if(_staged STREQUAL _after)
            continue()
        endif()
        if(NOT _staged STREQUAL _before)
            message(FATAL_ERROR
                "WindowsSignedOverlay: ${_candidate} is not the file that was signed "
                "(sha256 ${_staged}, the manifest has ${_before} before signing and ${_after} after)")
        endif()
        if(NOT EXISTS "${_signed}")
            message(FATAL_ERROR "WindowsSignedOverlay: the signed copy of ${_relative} does not exist: ${_signed}")
        endif()
        file(COPY_FILE "${_signed}" "${_candidate}" ONLY_IF_DIFFERENT)
        file(SHA256 "${_candidate}" _staged)
        if(NOT _staged STREQUAL _after)
            message(FATAL_ERROR
                "WindowsSignedOverlay: ${_signed} does not have the sha256 the manifest promises "
                "(${_staged}, expected ${_after})")
        endif()
    endforeach()

    if(_found EQUAL 0)
        message(FATAL_ERROR
            "WindowsSignedOverlay: ${_relative} is in the signing manifest but not in the ${CPACK_GENERATOR} "
            "staging directory ${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
    endif()
    math(EXPR _count "${_count} + 1")
endforeach()

# Every binary CPack staged is named by a row (a row whose signed file is "-" says the file is left as it is): a binary
# the signed stage lacks would otherwise ship unsigned, inside a package that is signed.
file(GLOB_RECURSE _binaries "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*.exe" "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*.dll")
foreach(_binary IN LISTS _binaries)
    # list(FIND), not IN_LIST: policy CMP0057 is not set in a CPack pre-build script, and without it CMake before 4.0
    # rejects IN_LIST as an unknown argument.
    list(FIND _covered "${_binary}" _covered_index)
    if(_covered_index EQUAL -1)
        message(FATAL_ERROR
            "WindowsSignedOverlay: ${_binary} is in the ${CPACK_GENERATOR} staging directory but not in the signing "
            "manifest, so nothing says it was signed, or left alone on purpose")
    endif()
endforeach()

message(STATUS
    "WindowsSignedOverlay: ${_count} files from the signing manifest in the ${CPACK_GENERATOR} staging")
