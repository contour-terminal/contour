include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/FilesystemResolver.cmake")

if(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    set(FREEBSD TRUE)
endif()

# --------------------------------------------------------------------------------------------------------
# crispy::core

option(STRONGHASH_USE_AES_NI "Build StrongHash with AES-NI support [default: ON]" ON)

set(crispy_SOURCES
    App.cpp App.h
    BufferObject.cpp BufferObject.h
    CLI.cpp CLI.h
    Comparison.h
    LRUCache.h
    StrongLRUCache.h
    StackTrace.cpp StackTrace.h
    TrieMap.h
    algorithm.h
    assert.h
    base64.h
    compose.h
    defines.h
    escape.h
    indexed.h
    logstore.h
    overloaded.h
    reference.h
    ring.h
    stdfs.h
    times.h
)

add_library(crispy-core ${crispy_SOURCES})
add_library(crispy::core ALIAS crispy-core)

set_target_properties(crispy-core PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")

if(STRONGHASH_USE_AES_NI)
    target_compile_definitions(crispy-core PUBLIC -DSTRONGHASH_USE_AES_NI=1)
endif()

if(MSVC)
    target_compile_definitions(crispy-core PUBLIC NOMINMAX)
endif()

set(CRISPY_CORE_LIBS range-v3::range-v3 fmt::fmt-header-only unicode::core Microsoft.GSL::GSL)
if(${USING_BOOST_FILESYSTEM})
    target_compile_definitions(crispy-core PUBLIC USING_BOOST_FILESYSTEM=1)
    list(APPEND CRISPY_CORE_LIBS Boost::filesystem)
else()
    list(APPEND CRISPY_CORE_LIBS ${FILESYSTEM_LIBS})
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL AMD64)
    target_compile_options(crispy-core PUBLIC -maes)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) # ARM64
    target_compile_options(crispy-core PUBLIC -march=armv8-a+fp+simd+crypto+crc)
else()
    message(WARNING "Target architecture not detected.")
endif()
target_compile_features(crispy-core PUBLIC cxx_std_17)
target_link_libraries(crispy-core PUBLIC ${CRISPY_CORE_LIBS})
target_include_directories(crispy-core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

macro(target_compile_definitions_if _target _visibility)
    foreach(_option IN ITEMS ${ARGN})
        if(${${_option}})
            message(STATUS "[${_target}] Adding compile definition: ${_option}")
            target_compile_definitions(${_target} ${_visibility} ${_option})
        else()
            message(STATUS "[${_target}] Skipping compile definition: ${_option}")
        endif()
    endforeach()
endmacro()

if(UNIX)
    include(CheckIncludeFiles)
    include(CheckIncludeFileCXX)
    include(CheckFunctionExists)

    check_function_exists(backtrace HAVE_BACKTRACE)
    check_function_exists(backtrace_symbols HAVE_BACKTRACE_SYMBOLS)
    check_function_exists(dladdr HAVE_DLADDR)
    check_function_exists(dlsym HAVE_DLSYM)
    check_include_files(dlfcn.h HAVE_DLFCN_H)
    check_include_files(cxxabi.h HAVE_CXXABI_H)
    check_include_files(execinfo.h HAVE_EXECINFO_H)
    check_include_files(sys/select.h HAVE_SYS_SELECT_H)
    check_include_files(unwind.h HAVE_UNWIND_H)

    # The following options are all important to get stacktrace dumping working
    target_link_libraries(crispy-core PUBLIC dl)
    if(NOT(FREEBSD))
        target_link_options(crispy-core PUBLIC -rdynamic -fno-pie)
    endif()
    if(NOT(APPLE OR FREEBSD))
        target_link_options(crispy-core PUBLIC -no-pie)
    endif()
    target_compile_definitions(crispy-core PUBLIC BOOST_STACKTRACE_USE_ADDR2LINE=1)
    if(CONTOUR_STACKTRACE_ADDR2LINE)
        target_compile_definitions(crispy-core PUBLIC CONTOUR_STACKTRACE_ADDR2LINE=1)
    endif()
    target_compile_definitions_if(crispy-core PUBLIC
        HAVE_BACKTRACE
        HAVE_BACKTRACE_SYMBOLS
        HAVE_CXXABI_H
        HAVE_DLADDR
        HAVE_DLFCN_H
        HAVE_DLSYM
        HAVE_EXECINFO_H
        HAVE_SYS_SELECT_H
        HAVE_UNWIND_H
    )
endif()

# --------------------------------------------------------------------------------------------------------
# crispy_test

option(CRISPY_TESTING "Enables building of unittests for crispy library [default: ON]" ON)
if(CRISPY_TESTING)
    enable_testing()
    add_executable(crispy_test
        BufferObject_test.cpp
        CLI_test.cpp
        LRUCache_test.cpp
        StrongLRUCache_test.cpp
        StrongLRUHashtable_test.cpp
        TrieMap_test.cpp
        base64_test.cpp
        indexed_test.cpp
        compose_test.cpp
        utils_test.cpp
        ring_test.cpp
        sort_test.cpp
        test_main.cpp
    )
    target_link_libraries(crispy_test fmt::fmt-header-only range-v3::range-v3 Catch2::Catch2 crispy::core)
    add_test(crispy_test ./crispy_test)
endif()
message(STATUS "[crispy] Compile unit tests: ${CRISPY_TESTING}")

