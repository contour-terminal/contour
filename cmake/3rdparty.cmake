macro(contour_add_fmtlib) # {{{
    # To download fmtlib at configure time we need a small trick: because
    # ExternalProject runs at build time, we create a fake minimal project and
    # build it. This will download and extract the sources.
    set(fmtlib_download_dir "${3rdparty_prefix}/fmtlib/fmtlib-download")
    set(fmtlib_source_dir "${3rdparty_prefix}/fmtlib/fmtlib-src")
    set(fmtlib_binary_dir "${3rdparty_prefix}/fmtlib/fmtlib-build")
    set(fmtlib-externalproject-in "${fmtlib_download_dir}/fmtlib-externalproject.cmake.in")
    if(NOT EXISTS "${fmtlib-externalproject-in}")
        file(WRITE "${fmtlib-externalproject-in}" [[
project(fmtlib-externalproject NONE)
include(ExternalProject)
ExternalProject_Add(fmtlib
        URL http://github.com/fmtlib/fmt/archive/${CONTOUR_EMBEDDED_FMTLIB_VERSION}.tar.gz
        URL_MD5 ${CONTOUR_EMBEDDED_FMTLIB_MD5SUM}
        SOURCE_DIR "${fmtlib_source_dir}"
        BINARY_DIR "${fmtlib_binary_dir}"
        UPDATE_DISCONNECTED 1
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        TEST_COMMAND "")
]])
    endif()
    configure_file("${fmtlib-externalproject-in}" "${fmtlib_download_dir}/CMakeLists.txt")
    execute_process(COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
            WORKING_DIRECTORY "${fmtlib_download_dir}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "'${CMAKE_COMMAND} -G ${CMAKE_GENERATOR} .' failed:\n${output}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build .
            WORKING_DIRECTORY "${fmtlib_download_dir}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "'${CMAKE_COMMAND} --build .' failed:\n${output}")
    endif()

    add_subdirectory("${fmtlib_source_dir}" "${fmtlib_binary_dir}" EXCLUDE_FROM_ALL)
endmacro() # }}}

include(ExternalProject)
macro(contour_add_range_v3) # {{{
    set(prefix "${CMAKE_BINARY_DIR}/deps")
    set(RANGE_V3_INCLUDE_DIR "${prefix}/include")

    ExternalProject_Add(range-v3-project
        PREFIX "${prefix}"
        DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/deps/downloads"
        DOWNLOAD_NAME range-v3-0.11.0.tar.gz
        URL https://github.com/ericniebler/range-v3/archive/0.11.0.tar.gz
        URL_HASH SHA256=376376615dbba43d3bef75aa590931431ecb49eb36d07bb726a19f680c75e20c
        CMAKE_COMMAND ${RANGE_V3_CMAKE_COMMAND}
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
                   -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                   -DBUILD_TESTING=OFF
                   -DRANGES_CXX_STD=${CMAKE_CXX_STANDARD}
                   -DRANGE_V3_DOCS=OFF
                   -DRANGE_V3_EXAMPLES=OFF
                   -DRANGE_V3_TESTS=OFF
                   -DRANGES_BUILD_CALENDAR_EXAMPLE=OFF
                   -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        BUILD_BYPRODUCTS "${RANGE_V3_INCLUDE_DIR}/range/v3/all.hpp"
    )

    add_library(range-v3 INTERFACE IMPORTED)
    file(MAKE_DIRECTORY ${RANGE_V3_INCLUDE_DIR})  # Must exist.
    set_target_properties(range-v3 PROPERTIES
        INTERFACE_COMPILE_OPTIONS "\$<\$<CXX_COMPILER_ID:MSVC>:/permissive->"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${RANGE_V3_INCLUDE_DIR}
        INTERFACE_INCLUDE_DIRECTORIES ${RANGE_V3_INCLUDE_DIR})
    add_dependencies(range-v3 range-v3-project)
endmacro() # }}}

macro(contour_add_yaml_cpp) # {{{
    # Same trick as in fmtlib.
    set(yaml-cpp_download_dir "${3rdparty_prefix}/yaml-cpp/yaml-cpp-download")
    set(yaml-cpp_source_dir "${3rdparty_prefix}/yaml-cpp/yaml-cpp-src")
    set(yaml-cpp_binary_dir "${3rdparty_prefix}/yaml-cpp/yaml-cpp-build")
    set(yaml-cpp-externalproject-in "${yaml-cpp_download_dir}/yaml-cpp-externalproject.cmake.in")
    if(NOT EXISTS "${yaml-cpp-externalproject-in}")
        file(WRITE "${yaml-cpp-externalproject-in}" [[
project(yaml-cpp-externalproject NONE)
include(ExternalProject)
ExternalProject_Add(yaml-cpp
        URL http://github.com/jbeder/yaml-cpp/archive/yaml-cpp-${CONTOUR_EMBEDDED_YAML_CPP_VERSION}.tar.gz
        URL_MD5 ${CONTOUR_EMBEDDED_YAML_CPP_MD5SUM}
        SOURCE_DIR "${yaml-cpp_source_dir}"
        BINARY_DIR "${yaml-cpp_binary_dir}"
        UPDATE_DISCONNECTED 1
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        TEST_COMMAND "")
]])
    endif()

    configure_file("${yaml-cpp-externalproject-in}" "${yaml-cpp_download_dir}/CMakeLists.txt")
    execute_process(COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
                    WORKING_DIRECTORY "${yaml-cpp_download_dir}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "'${CMAKE_COMMAND} -G ${CMAKE_GENERATOR} .' failed:\n${output}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build .
                    WORKING_DIRECTORY "${yaml-cpp_download_dir}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "'${CMAKE_COMMAND} --build .' failed:\n${output}")
    endif()

    # Patch it: don't produce warnings on yaml-cpp files and don't build tests.
    set(yaml_cpp_patch "${3rdparty_prefix}/patches/yaml_cpp.patch")
    if(NOT EXISTS "${yaml_cpp_patch}")
        file(WRITE "${yaml_cpp_patch}" [[
--- CMakeLists.txt	2021-03-03 08:33:57.271688830 +0100
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
        if(NOT CONTOUR_PATCH_PROGRAM)
            find_program(CONTOUR_PATCH_PROGRAM patch DOC "Patch program") # todo: version, windows
            if(CONTOUR_PATCH_PROGRAM_NOTFOUND)
                message(FATAL_ERROR "Patch program is required")
            endif()
        endif()
        execute_process(COMMAND "${CONTOUR_PATCH_PROGRAM}" -i "${yaml_cpp_patch}"
                WORKING_DIRECTORY "${yaml-cpp_source_dir}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
        if(result)
            message(FATAL_ERROR "'${CONTOUR_PATCH_PROGRAM} -i <${yaml_cpp_patch}' failed:\n${output}")
        endif()
    endif()
    add_subdirectory("${yaml-cpp_source_dir}" "${yaml-cpp_binary_dir}" EXCLUDE_FROM_ALL)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endmacro() # }}}

macro(contour_add_libunicode) # {{{
    # Same trick as in fmtlib.
    set(libunicode_download_dir "${3rdparty_prefix}/libunicode/libunicode-download")
    set(libunicode_source_dir "${3rdparty_prefix}/libunicode/libunicode-src")
    set(libunicode_binary_dir "${3rdparty_prefix}/libunicode/libunicode-build")
    set(libunicode-externalproject-in "${libunicode_download_dir}/libunicode-externalproject.cmake.in")
    if(NOT EXISTS "${libunicode-externalproject-in}")
        file(WRITE "${libunicode-externalproject-in}" [[
project(libunicode-externalproject NONE)
include(ExternalProject)
ExternalProject_Add(libunicode
        URL http://github.com/christianparpart/libunicode/tarball/${CONTOUR_EMBEDDED_LIBUNICODE_VERSION}
        # SKIP URL_MD5
        SOURCE_DIR "${libunicode_source_dir}"
        BINARY_DIR "${libunicode_binary_dir}"
        UPDATE_DISCONNECTED 1
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        TEST_COMMAND "")
]])
    endif()
    configure_file("${libunicode-externalproject-in}" "${libunicode_download_dir}/CMakeLists.txt")
    execute_process(COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
            WORKING_DIRECTORY "${libunicode_download_dir}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "'${CMAKE_COMMAND} -G ${CMAKE_GENERATOR} .' failed:\n${output}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build .
            WORKING_DIRECTORY "${libunicode_download_dir}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(result)
        message(FATAL_ERROR "'${CMAKE_COMMAND} --build .' failed:\n${output}")
    endif()

    # XXX: temporary patch until libunicode gets rid of sumbodules.
    set(libunicode_patch "${3rdparty_prefix}/patches/libunicode.patch")
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
        if(NOT CONTOUR_PATCH_PROGRAM)
            find_program(CONTOUR_PATCH_PROGRAM patch DOC "Patch program")
            if(CONTOUR_PATCH_PROGRAM_NOTFOUND)
                message(FATAL_ERROR "Patch program is required")
            endif()
        endif()
        execute_process(COMMAND "${CONTOUR_PATCH_PROGRAM}" -i "${libunicode_patch}"
                WORKING_DIRECTORY "${libunicode_source_dir}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
        if(result)
            message(FATAL_ERROR "'${CONTOUR_PATCH_PROGRAM} -i ${libunicode_patch}' failed:\n${output}")
        endif()
    endif()
    add_subdirectory("${libunicode_source_dir}" "${libunicode_binary_dir}" EXCLUDE_FROM_ALL)
endmacro() # }}}

macro(contour_add_catch2)
    file(DOWNLOAD http://raw.githubusercontent.com/catchorg/Catch2/v${CONTOUR_EMBEDDED_CATCH2_VERSION}/single_include/catch2/catch.hpp
            "${3rdparty_prefix}/catch2/catch.hpp"
            #SHOW_PROGRESS
            EXPECTED_MD5 ${CONTOUR_EMBEDDED_CATCH2_MD5SUM})
    add_library(Catch2 INTERFACE)
    target_include_directories(Catch2 SYSTEM INTERFACE "${3rdparty_prefix}")
    add_library(Catch2::Catch2 ALIAS Catch2)
endmacro()
