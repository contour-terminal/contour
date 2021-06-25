set(THIRDPARTIES_HAS_FETCHCONTENT ON)
# if(${CMAKE_VERSION} VERSION_LESS 3.11)
#     set(THIRDPARTIES_HAS_FETCHCONTENT OFF)
# endif()

if(THIRDPARTIES_HAS_FETCHCONTENT)
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)
else()
    include(DownloadProject)
endif()

if(NOT FETCHCONTENT_BASE_DIR STREQUAL "")
    set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}/3rdparty")
endif()

set(3rdparty_DOWNLOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/_downloads" CACHE FILEPATH "3rdparty download directory.")
message(STATUS "base dir: ${FETCHCONTENT_BASE_DIR}")
message(STATUS "dnld dir: ${3rdparty_DOWNLOAD_DIR}")

macro(ThirdPartiesAdd_fmtlib)
    set(3rdparty_fmtlib_VERSION "8.0.0" CACHE STRING "fmtlib version")
    set(3rdparty_fmtlib_CHECKSUM "SHA256=7bce0e9e022e586b178b150002e7c2339994e3c2bbe44027e9abb0d60f9cce83" CACHE STRING "fmtlib checksum")
    set(3rdparty_fmtlib_URL "https://github.com/fmtlib/fmt/archive/refs/tags/${3rdparty_fmtlib_VERSION}.tar.gz")
    if(THIRDPARTIES_HAS_FETCHCONTENT)
        FetchContent_Declare(
            fmtlib
            URL "${3rdparty_fmtlib_URL}"
            URL_HASH "${3rdparty_fmtlib_CHECKSUM}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "fmtlib-${3rdparty_fmtlib_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(fmtlib)
    else()
        download_project(
            PROJ fmtlib
            URL "${3rdparty_fmtlib_URL}"
            URL_HASH "${3rdparty_fmtlib_CHECKSUM}"
            PREFIX "${FETCHCONTENT_BASE_DIR}/fmtlib-${3rdparty_fmtlib_VERSION}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "fmtlib-${3rdparty_fmtlib_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
            UPDATE_DISCONNECTED 1
        )
    endif()
endmacro()

macro(ThirdPartiesAdd_Catch2)
    set(3rdparty_Catch2_VERSION "2.13.6" CACHE STRING "Embedded catch2 version")
    set(3rdparty_Catch2_CHECKSUM "SHA256=48dfbb77b9193653e4e72df9633d2e0383b9b625a47060759668480fdf24fbd4" CACHE STRING "Embedded catch2 checksum")
    set(3rdparty_Catch2_URL "https://github.com/catchorg/Catch2/archive/refs/tags/v${3rdparty_Catch2_VERSION}.tar.gz")
    set(CATCH_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXTRA_TESTS OFF CACHE INTERNAL "")
    set(CATCH_BUILD_TESTING OFF CACHE INTERNAL "")
    set(CATCH_ENABLE_WERROR OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_HELPERS OFF CACHE INTERNAL "")
    if(THIRDPARTIES_HAS_FETCHCONTENT)
        FetchContent_Declare(
            Catch2
            URL "${3rdparty_Catch2_URL}"
            URL_HASH "${3rdparty_Catch2_CHECKSUM}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "catch2-${3rdparty_Catch2_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(Catch2)
    else()
        download_project(
            PROJ Catch2
            URL "${3rdparty_Catch2_URL}"
            URL_HASH "${3rdparty_Catch2_CHECKSUM}"
            PREFIX "${FETCHCONTENT_BASE_DIR}/Catch2-${3rdparty_Catch2_VERSION}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "catch2-${3rdparty_Catch2_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
        )
    endif()
endmacro()

macro(ThirdPartiesAdd_range_v3)
    set(3rdparty_range_v3_VERSION "0.11.0" CACHE STRING "Embedded range-v3 version")
    set(3rdparty_range_v3_CHECKSUM "MD5=97ab1653f3aa5f9e3d8200ee2a4911d3" CACHE STRING "Embedded range-v3 hash")
    if(THIRDPARTIES_HAS_FETCHCONTENT)
        FetchContent_GetProperties(range_v3)
        if(NOT "${range_v3_POPULATED}")
            FetchContent_Populate(
                range_v3
                URL "https://github.com/ericniebler/range-v3/archive/${3rdparty_range_v3_VERSION}.tar.gz"
                URL_HASH "${3rdparty_range_v3_CHECKSUM}"
                DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
                DOWNLOAD_NAME "range-v3-${3rdparty_range_v3_VERSION}.tar.gz"
                EXCLUDE_FROM_ALL
            )
            add_subdirectory(${range_v3_SOURCE_DIR} ${range_v3_BINARY_DIR} EXCLUDE_FROM_ALL)
            # ^^^ That's the only way to avoid installing this dependency during install step.
        endif()
    else()
        download_project(
            PROJ range-v3
            URL "https://github.com/ericniebler/range-v3/archive/${3rdparty_range_v3_VERSION}.tar.gz"
            URL_HASH "${3rdparty_range_v3_CHECKSUM}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "range-v3-${3rdparty_range_v3_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
            PREFIX "${FETCHCONTENT_BASE_DIR}/range-v3-${3rdparty_range_v3_VERSION}"
        )
    endif()
endmacro()

# {{{ yaml-cpp
macro(ThirdPartiesAdd_yaml_cpp)
    set(3rdparty_yaml_cpp_VERSION "0.6.3" CACHE STRING "Embedded yaml-cpp version")
    set(3rdparty_yaml_cpp_CHECKSUM "MD5=b45bf1089a382e81f6b661062c10d0c2" CACHE STRING "Embedded yaml-cpp checksum")
    #set(yaml_cpp_patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/yaml-cpp.patch")
    set(yaml_cpp_patch "${CMAKE_CURRENT_BINARY_DIR}/patches/yaml-cpp.patch")
    if(NOT EXISTS "${yaml_cpp_patch}")
        file(WRITE "${yaml_cpp_patch}"
[[--- CMakeLists.txt	2021-03-03 08:33:57.271688830 +0100
+++ CMakeLists.txt.new	2021-03-03 09:32:34.817113397 +0100
@@ -15,8 +15,8 @@
 ### Project options
 ###
 ## Project stuff
-option(YAML_CPP_BUILD_TESTS "Enable testing" ON)
-option(YAML_CPP_BUILD_TOOLS "Enable parse tools" ON)
+option(YAML_CPP_BUILD_TESTS "Enable testing" OFF)
+option(YAML_CPP_BUILD_TOOLS "Enable parse tools" OFF)
 option(YAML_CPP_BUILD_CONTRIB "Enable contrib stuff in library" ON)
 option(YAML_CPP_INSTALL "Enable generation of install target" ON)

@@ -259,7 +259,7 @@
 endif()

 if (NOT CMAKE_VERSION VERSION_LESS 2.8.12)
-    target_include_directories(yaml-cpp
+  target_include_directories(yaml-cpp SYSTEM
         PUBLIC $<BUILD_INTERFACE:${YAML_CPP_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${INCLUDE_INSTALL_ROOT_DIR}>
         PRIVATE $<BUILD_INTERFACE:${YAML_CPP_SOURCE_DIR}/src>)
]])
    endif()
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE INTERNAL "")
    set(YAML_CPP_BUILD_TESTS OFF CACHE INTERNAL "")
    set(YAML_CPP_BUILD_TOOLS OFF CACHE INTERNAL "")
    set(YAML_CPP_INSTALL OFF CACHE INTERNAL "")
    if(THIRDPARTIES_HAS_FETCHCONTENT)
        FetchContent_Declare(
            yaml-cpp
            URL "http://github.com/jbeder/yaml-cpp/archive/yaml-cpp-${3rdparty_yaml_cpp_VERSION}.tar.gz"
            URL_HASH "${3rdparty_yaml_cpp_CHECKSUM}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "yaml-cpp-${3rdparty_yaml_cpp_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
            # PATCH_COMMAND breaks on GitHub CI for Windows (but not on local Windows machine, what?)
            # PATCH_COMMAND patch "${yaml_cpp_patch}"
        )
        FetchContent_MakeAvailable(yaml-cpp)
    else()
        download_project(
            PROJ yaml-cpp
            URL "http://github.com/jbeder/yaml-cpp/archive/yaml-cpp-${3rdparty_yaml_cpp_VERSION}.tar.gz"
            URL_HASH "${3rdparty_yaml_cpp_CHECKSUM}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "yaml-cpp-${3rdparty_yaml_cpp_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
            PREFIX "${FETCHCONTENT_BASE_DIR}/yaml-cpp-${3rdparty_range_v3_VERSION}"
            # PATCH_COMMAND breaks on GitHub CI for Windows (but not on local Windows machine, what?)
            # PATCH_COMMAND patch "${yaml_cpp_patch}"
        )
    endif()
endmacro()
# }}}

# {{{ libunicode
macro(ThirdPartiesAdd_libunicode)
    set(3rdparty_libunicode_VERSION "8ae215269247a74e10b2ed90de023343cd447be5" CACHE STRING "libunicode: commit hash")
    set(3rdparty_libunicode_CHECKSUM "SHA256=256a22b22ee724cc6e61049e8649468697a2e2d83a8c7a6b2714ec36acdb59b2" CACHE STRING "libunicode: download checksum")
    # XXX: temporary patch until libunicode gets rid of sumbodules.
    set(libunicode_patch "${CMAKE_CURRENT_BINARY_DIR}/patches/libunicode.patch")
    if(NOT EXISTS "${libunicode_patch}")
        file(WRITE "${libunicode_patch}" [[
--- CMakeLists.txt	2021-03-03 08:40:11.184332244 +0100
+++ CMakeLists.txt.new	2021-03-03 08:49:06.336734764 +0100
@@ -48,18 +48,6 @@
 # ----------------------------------------------------------------------------
 # 3rdparty dependencies

-if(LIBUNICODE_EMBEDDED_FMTLIB)
-    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/fmt" EXCLUDE_FROM_ALL)
-    add_definitions(-DFMT_USE_WINDOWS_H=0)
-else()
-    # master project must provide its own fmtlib
-endif()
-
-if(LIBUNICODE_TESTING AND LIBUNICODE_EMBEDDED_CATCH2)
-    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/catch2")
-else()
-    # master project must provide its own fmtlib
-endif()

 add_subdirectory(src/unicode)
 add_subdirectory(src/tools)
]])
    endif()
    if(THIRDPARTIES_HAS_FETCHCONTENT)
        FetchContent_Declare(
            libunicode
            URL "https://github.com/christianparpart/libunicode/archive/${3rdparty_libunicode_VERSION}.tar.gz"
            URL_HASH "${3rdparty_libunicode_CHECKSUM}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "libunicode-${3rdparty_libunicode_VERSION}.tar.gz"
            UPDATE_DISCONNECTED 0
            EXCLUDE_FROM_ALL
            # same here
            #PATCH_COMMAND patch "${libunicode_patch}"
        )
        FetchContent_MakeAvailable(libunicode)
    else()
        download_project(
            PROJ libunicode
            URL "https://github.com/christianparpart/libunicode/archive/${3rdparty_libunicode_VERSION}.zip"
            URL_HASH "${3rdparty_libunicode_CHECKSUM}"
            DOWNLOAD_DIR "${3rdparty_DOWNLOAD_DIR}"
            DOWNLOAD_NAME "libunicode-${3rdparty_libunicode_VERSION}.tar.gz"
            EXCLUDE_FROM_ALL
            PREFIX "${FETCHCONTENT_BASE_DIR}/libunicode-${3rdparty_libunicode_VERSION}"
            # same here
            #PATCH_COMMAND patch "${libunicode_patch}"
        )
    endif()
endmacro()
# }}}

