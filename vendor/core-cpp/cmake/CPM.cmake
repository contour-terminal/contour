# SPDX-License-Identifier: Apache-2.0
#
# CPM.cmake bootstrap: downloads the pinned CPM once, verifies it and includes it.
# https://github.com/cpm-cmake/CPM.cmake
#
# core_cpp_resolve_dependencies() includes this only when a dependency has to be
# fetched and the parent project has not already loaded CPM. A parent that
# provides every dependency, or that sets CORE_CPP_FETCH_DEPS=OFF, never reaches it.
#
# The pin (0.40.8 and its SHA-256) is endo's, from cmake/EndoThirdParties.cmake at
# f774a210. The download shape is fastcached's cmake/CPM.cmake at eb9c9c68: a
# bounded, status-checked transfer, with every argument quoted.
#
# The bound on stalled transfers comes from FetchTransferBound.cmake, which exports
# environment variables to every git clone of the configure. That is process-wide
# state, so only CoreCppTopLevel.cmake includes it, before any dependency is
# resolved. As a subproject, core-cpp's download is bounded by the parent's
# FASTCACHED_FETCH_SILENCE_SECONDS when the parent defines it, and is otherwise
# unbounded. An empty INACTIVITY_TIMEOUT would break the argument list, so none is
# passed.
set(_coreCppCpmBound "")
if(DEFINED FASTCACHED_FETCH_SILENCE_SECONDS)
    set(_coreCppCpmBound INACTIVITY_TIMEOUT "${FASTCACHED_FETCH_SILENCE_SECONDS}")
endif()

set(CPM_DOWNLOAD_VERSION 0.40.8)
set(CPM_HASH_SUM "78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791")
set(CPM_DOWNLOAD_URL
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake")

if(CPM_SOURCE_CACHE)
    set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
    set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

# Expand a relative path, or one that starts with a tilde.
get_filename_component(CPM_DOWNLOAD_LOCATION "${CPM_DOWNLOAD_LOCATION}" ABSOLUTE)

# `INACTIVITY_TIMEOUT` rather than `TIMEOUT`: the bound is on silence, so a slow
# download that keeps delivering still completes. `STATUS` because a failed
# `file(DOWNLOAD)` without it is silent and leaves a truncated file behind, which
# the `include()` below would then report as a syntax error in a file nobody wrote.
file(DOWNLOAD
    "${CPM_DOWNLOAD_URL}"
    "${CPM_DOWNLOAD_LOCATION}"
    EXPECTED_HASH "SHA256=${CPM_HASH_SUM}"
    ${_coreCppCpmBound}
    STATUS cpmDownloadStatus
)
list(GET cpmDownloadStatus 0 cpmDownloadCode)
if(NOT cpmDownloadCode EQUAL 0)
    message(FATAL_ERROR
        "could not download the CPM.cmake bootstrap: ${cpmDownloadStatus}\n"
        "  from: ${CPM_DOWNLOAD_URL}\n"
        "  into: ${CPM_DOWNLOAD_LOCATION}\n"
        "Re-run the configure, point CPM_SOURCE_CACHE at a directory that already holds "
        "the bootstrap, or provide every dependency and set CORE_CPP_FETCH_DEPS=OFF. If the "
        "transfer stalled, cmake/FetchTransferBound.cmake is what abandoned it and why.")
endif()

include("${CPM_DOWNLOAD_LOCATION}")
