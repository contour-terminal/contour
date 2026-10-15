# SPDX-License-Identifier: Apache-2.0

# core::base: the headers directly in src/core/, namespace core. It owns core/Config.hpp, which
# CMake generates when it configures core-cpp. Generator.hpp is the one header here needing
# coroutine language support; it needs nothing else, which is why it moved here rather than
# staying in core::async (Task A5b).
set(_coreCppBaseLibs "")
if(CORE_CPP_USE_THREADS)
    list(APPEND _coreCppBaseLibs Threads::Threads)
endif()
# PUBLIC, because <core/Profiling.hpp> includes <tracy/Tracy.hpp> when CORE_CPP_WITH_TRACY is on.
if(CORE_CPP_WITH_TRACY)
    list(APPEND _coreCppBaseLibs Tracy::TracyClient)
endif()

core_cpp_add_module(base KIND STATIC
    HEADERS
        Assert.hpp
        Base64.hpp
        Deferred.hpp
        Defines.hpp
        Environment.hpp
        Escape.hpp
        Flags.hpp
        FNV.hpp
        Generator.hpp
        Overloaded.hpp
        Profiling.hpp
        Ranges.hpp
        Sha256.hpp
        Times.hpp
        UserInfo.hpp
        Utf8Path.hpp
        Utils.hpp
        # Windows only, and on every other platform a header that declares nothing: it says why.
        WideString.hpp
        "${CORE_CPP_GENERATED_INCLUDE_DIR}/core/Config.hpp"
    SOURCES
        Environment.cpp
        UserInfo.cpp
        Utf8Path.cpp
        Utils.cpp
    # The conversions between UTF-16 and UTF-8 behind every wide Win32 call, core-cpp's own and its
    # consumers'.
    SOURCES_WINDOWS
        WideString.cpp
    PUBLIC_LIBS ${_coreCppBaseLibs})

core_cpp_add_test(base
    SOURCES
        Base64_test.cpp
        Environment_test.cpp
        Escape_test.cpp
        Flags_test.cpp
        FNV_test.cpp
        Generator_test.cpp
        Profiling_test.cpp
        Ranges_test.cpp
        Sha256_test.cpp
        Times_test.cpp
        Utf8Path_test.cpp
        Utils_test.cpp
    SOURCES_WINDOWS
        WideString_test.cpp)
