if(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    set(FREEBSD TRUE)
endif()

# --------------------------------------------------------------------------------------------------------
# crispy::core

option(STRONGHASH_USE_INTRINSICS "Build StrongHash with AES-NI (x86-64) / NEON (ARM64) support [default: ON]" ON)

set(crispy_SOURCES
    App.cpp App.hpp
    BufferObject.cpp BufferObject.hpp
    CLI.cpp CLI.hpp
    Comparison.hpp
    LRUCache.hpp
    StrongLRUCache.hpp
    ScopedTimer.hpp
    StackTrace.cpp StackTrace.hpp
    TrieMap.hpp
    AlignedAllocator.hpp
    ASCII.hpp
    Assert.hpp
    Base64.hpp
    Compose.hpp
    Defines.hpp
    Environment.cpp Environment.hpp
    Escape.hpp
    FileDescriptor.hpp
    Flags.hpp
    InterpolatedString.cpp InterpolatedString.hpp
    LogSink.cpp LogSink.hpp
    LogStore.cpp LogStore.hpp
    Overloaded.hpp
    Reference.hpp
    Ring.hpp
    testing/Environment.hpp
    Times.hpp
    UserInfo.cpp UserInfo.hpp
    Utils.cpp Utils.hpp
)

add_library(crispy-core STATIC ${crispy_SOURCES})
add_library(crispy::core ALIAS crispy-core)

if(STRONGHASH_USE_AES_NI)
    target_compile_definitions(crispy-core PUBLIC -DSTRONGHASH_USE_AES_NI=1)
endif()

if(MSVC)
    target_compile_definitions(crispy-core PUBLIC NOMINMAX)
endif()

set(CRISPY_CORE_LIBS unicode::unicode Microsoft.GSL::GSL boxed-cpp::boxed-cpp reflection-cpp::reflection-cpp)

# if compiler is not MSVC
if(NOT MSVC)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL AMD64)
        target_compile_options(crispy-core PUBLIC -maes)
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) # ARM64
        target_compile_options(crispy-core PUBLIC -march=armv8-a+fp+simd+crypto+crc)
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL ARM64)
        message(STATUS "Target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    else()
        message(WARNING "Target architecture not detected: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()

target_compile_features(crispy-core PUBLIC cxx_std_17)
# Threads::Threads is PUBLIC because threading is part of crispy-core's INTERFACE and not merely its
# implementation: BufferObject.hpp, LogSink.hpp, ReadSelector.hpp, FileDescriptor.hpp and
# Environment.hpp all expose std::mutex / std::condition_variable / std::thread, so every consumer
# instantiates them and every consumer needs the threading library on its link line.
#
# On glibc that link is free -- pthread_create has lived in libc since glibc 2.34 -- so no Linux job
# ever noticed it missing. FreeBSD keeps those symbols in libthr, where the omission is fatal:
# `ld: error: undefined symbol: pthread_create`, linking crispy_test. Declaring it here rather than
# on each test target also carries it to vtpty, vthost and vtrasterizer, which reach threading
# through this same header set.
target_link_libraries(crispy-core PUBLIC ${CRISPY_CORE_LIBS} Threads::Threads)
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
    target_link_libraries(crispy-core PUBLIC ${CMAKE_DL_LIBS})
    if(NOT(FREEBSD) AND NOT ENABLE_STATIC_LINKING)
        target_link_options(crispy-core PUBLIC -rdynamic)
    endif()
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

option(CRISPY_TESTING "Enables building of unittests for crispy library [default: ON]" ${CONTOUR_TESTING})
if(CRISPY_TESTING)
    enable_testing()
    add_executable(crispy_test
        test_main.cpp
        BufferObject_test.cpp
        CLI_test.cpp
        LRUCache_test.cpp
        StrongLRUCache_test.cpp
        StrongLRUHashtable_test.cpp
        TrieMap_test.cpp
        Base64_test.cpp
        Compose_test.cpp
        Environment_test.cpp
        InterpolatedString_test.cpp
        LogSink_test.cpp
        Utils_test.cpp
        Ring_test.cpp
        Sort_test.cpp
        Times_test.cpp
    )
    # read_selector is a POSIX select()/epoll() construct (uses fd_set, sys/select.h); its header is
    # only ever compiled through the UNIX-gated UnixPty. The test likewise only builds off Windows,
    # where MSVC would otherwise choke on the unavailable fd_set type.
    if(NOT WIN32)
        target_sources(crispy_test PRIVATE ReadSelector_test.cpp)
    endif()
    target_link_libraries(crispy_test Catch2::Catch2 crispy::core)
    add_test(NAME crispy_test COMMAND $<TARGET_FILE:crispy_test>)

    if(NOT WIN32)
        target_compile_options(crispy_test PRIVATE -Wno-c2y-extensions)
    endif()

endif()
message(STATUS "[crispy] Compile unit tests: ${CRISPY_TESTING}")
