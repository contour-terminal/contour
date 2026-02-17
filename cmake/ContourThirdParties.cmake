# This directory structure is being created by `scripts/install-deps.sh`
# and is used to inject all the dependencies the operating system's
# package manager did not provide (not found or too old version).

# {{{ helper: subproject_version(<subproject-name> <result-variable>)
#
# Extract version of a sub-project, which was previously included with add_subdirectory().
function(subproject_version subproject_name VERSION_VAR)
    # Read CMakeLists.txt for subproject and extract project() call(s) from it.
    file(STRINGS "${${subproject_name}_SOURCE_DIR}/CMakeLists.txt" project_calls REGEX "[ \t]*project\\(")
    # For every project() call try to extract its VERSION option
    foreach(project_call ${project_calls})
        string(REGEX MATCH "VERSION[ ]+\"?([^ \")]+)" version_param "${project_call}")
        if(version_param)
            set(version_value "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(version_value)
        set(${VERSION_VAR} "${version_value}" PARENT_SCOPE)
    else()
        message("WARNING: Cannot extract version for subproject '${subproject_name}'")
    endif()
endfunction(subproject_version)
# }}}

set(ContourThirdParties_SRCDIR ${PROJECT_SOURCE_DIR}/_deps/sources)
if(EXISTS "${ContourThirdParties_SRCDIR}/CMakeLists.txt")
    message(STATUS "Embedding 3rdparty libraries: ${ContourThirdParties_SRCDIR}")
    add_subdirectory(${ContourThirdParties_SRCDIR})
else()
    message(STATUS "No 3rdparty libraries found at ${ContourThirdParties_SRCDIR}")
endif()

macro(HandleThirdparty _TARGET _CPM_TARGET)
    cmake_parse_arguments(_HTP "" "VERSION" "" ${ARGN})
    if(TARGET ${_TARGET})
        set(THIRDPARTY_BUILTIN_${_TARGET} "embedded")
    else()
        if(_HTP_VERSION)
            find_package(${_TARGET} ${_HTP_VERSION} QUIET)
        else()
            find_package(${_TARGET} QUIET)
        endif()
        if(${_TARGET}_FOUND)
            set(THIRDPARTY_BUILTIN_${_TARGET} "system package")
        elseif(CONTOUR_USE_CPM)
            message(STATUS "Using CPM to fetch ${_TARGET}")
            CPMAddPackage(${_CPM_TARGET})
            set(THIRDPARTY_BUILTIN_${_TARGET} "embedded (CPM)")
        else()
            message(FATAL_ERROR "Could not find ${_TARGET}. "
                "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
        endif()
    endif()
endmacro()


message(STATUS "==============================================================================")
message(STATUS "    Contour ThirdParties: ${ContourThirdParties}")

set(LIBUNICODE_MINIMAL_VERSION "0.8.0")
set(BOXED_CPP_MINIMAL_VERSION "1.4.3")
set(TERMBENCH_PRO_COMMIT_HASH "f6c37988e6481b48a8b8acaf1575495e018e9747")
set(CATCH_VERSION "3.4.0")
set(YAML_CPP_VERSION "0.8.0")
set(HARFBUZZ_VERSION "8.4.0")
set(GSL_VERSION "3.1.0")
set(REFLECTION_CPP_VERSION "0.4.0")
set(FREETYPE_VERSION "2.10.0")


if(TARGET GSL)
    set(THIRDPARTY_BUILTIN_GSL "embedded")
else()
    if(WIN32)
        find_package(Microsoft.GSL CONFIG QUIET)
    else()
        find_package(Microsoft.GSL QUIET)
    endif()
    if(Microsoft.GSL_FOUND)
        set(THIRDPARTY_BUILTIN_GSL "system package")
    elseif(CONTOUR_USE_CPM)
        message(STATUS "Using CPM to fetch GSL")
        CPMAddPackage(
            NAME GSL
            GITHUB_REPOSITORY microsoft/GSL
            GIT_TAG v${GSL_VERSION}
            OPTIONS "GSL_TEST=OFF"
        )
        set(THIRDPARTY_BUILTIN_GSL "embedded (CPM)")
    else()
        message(FATAL_ERROR "Could not find GSL. "
            "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
    endif()
endif()



if(CONTOUR_TESTING)
    HandleThirdparty(Catch2 "gh:catchorg/Catch2@${CATCH_VERSION}" VERSION ${CATCH_VERSION})
endif()
HandleThirdparty(yaml-cpp "gh:jbeder/yaml-cpp#${YAML_CPP_VERSION}" VERSION ${YAML_CPP_VERSION})
HandleThirdparty(Freetype "https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VERSION}.tar.gz" VERSION ${FREETYPE_VERSION})
# harfbuzz is only needed for non static builds since qt provides own harfbuzz target
HandleThirdparty(HarfBuzz "gh:harfbuzz/harfbuzz#${HARFBUZZ_VERSION}" VERSION ${HARFBUZZ_VERSION})

if(COMMAND ContourThirdParties_Embed_libunicode)
    ContourThirdParties_Embed_libunicode()
    subproject_version(libunicode libunicode_version)
    if(NOT DEFINED libunicode_version OR libunicode_version VERSION_LESS LIBUNICODE_MINIMAL_VERSION)
        message(FATAL_ERROR "Embedded libunicode version must be at least ${LIBUNICODE_MINIMAL_VERSION}, but found ${libunicode_version}")
    endif()
    set(THIRDPARTY_BUILTIN_unicode_core "embedded")
else()
    HandleThirdparty(libunicode "gh:contour-terminal/libunicode#v${LIBUNICODE_MINIMAL_VERSION}")
endif()

if(LIBTERMINAL_BUILD_BENCH_HEADLESS)
    if(COMMAND ContourThirdParties_Embed_termbench_pro)
        ContourThirdParties_Embed_termbench_pro()
    endif()
    if(TARGET termbench)
        set(THIRDPARTY_BUILTIN_termbench "embedded")
    else()
        find_package(termbench-pro QUIET)
        if(termbench-pro_FOUND)
            set(THIRDPARTY_BUILTIN_termbench "system package")
        elseif(CONTOUR_USE_CPM)
            message(STATUS "Using CPM to fetch termbench-pro")
            CPMAddPackage(
                NAME termbench-pro
                GITHUB_REPOSITORY contour-terminal/termbench-pro
                GIT_TAG ${TERMBENCH_PRO_COMMIT_HASH}
            )
            set(THIRDPARTY_BUILTIN_termbench "embedded (CPM)")
        else()
            message(FATAL_ERROR "Could not find termbench-pro. "
                "Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON.")
        endif()
    endif()
else()
    set(THIRDPARTY_BUILTIN_termbench "(bench-headless disabled)")
endif()

if(COMMAND ContourThirdParties_Embed_boxed_cpp)
    ContourThirdParties_Embed_boxed_cpp()
    subproject_version(boxed-cpp boxed_cpp_version)
    if(NOT DEFINED boxed_cpp_version OR boxed_cpp_version VERSION_LESS BOXED_CPP_MINIMAL_VERSION)
        message(FATAL_ERROR "Embedded boxed-cpp version must be at least ${BOXED_CPP_MINIMAL_VERSION}, but found ${boxed_cpp_version}")
    endif()
    set(THIRDPARTY_BUILTIN_boxed-cpp "embedded")
else()
    HandleThirdparty(boxed-cpp "gh:contour-terminal/boxed-cpp#v${BOXED_CPP_MINIMAL_VERSION}")
endif()

if(COMMAND ContourThirdParties_Embed_reflection_cpp)
    ContourThirdParties_Embed_reflection_cpp()
    set(THIRDPARTY_BUILTIN_reflection-cpp "embedded")
else()
    HandleThirdparty(reflection-cpp "gh:contour-terminal/reflection-cpp#v${REFLECTION_CPP_VERSION}")
endif()

macro(ContourThirdPartiesSummary2)
    message(STATUS "==============================================================================")
    message(STATUS "    Contour ThirdParties")
    message(STATUS "------------------------------------------------------------------------------")
    message(STATUS "Catch2              ${THIRDPARTY_BUILTIN_Catch2}")
    message(STATUS "GSL                 ${THIRDPARTY_BUILTIN_GSL}")
    message(STATUS "freetype            ${THIRDPARTY_BUILTIN_Freetype}")
    message(STATUS "harfbuzz            ${THIRDPARTY_BUILTIN_HarfBuzz}")
    message(STATUS "yaml-cpp            ${THIRDPARTY_BUILTIN_yaml-cpp}")
    message(STATUS "termbench-pro       ${THIRDPARTY_BUILTIN_termbench}")
    message(STATUS "reflection-cpp      ${THIRDPARTY_BUILTIN_reflection-cpp}")
    if(DEFINED THIRDPARTY_BUILTIN_libunicode)
        message(STATUS "libunicode          ${THIRDPARTY_BUILTIN_libunicode}")
    else()
        message(STATUS "libunicode          ${THIRDPARTY_BUILTIN_unicode_core}")
    endif()
    message(STATUS "boxed-cpp           ${THIRDPARTY_BUILTIN_boxed-cpp}")
    message(STATUS "------------------------------------------------------------------------------")
endmacro()
