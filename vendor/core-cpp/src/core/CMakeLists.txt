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

# core::systemRandom() needs a platform that has a source for it (see SOURCES_POSIX, SOURCES_EMSCRIPTEN
# and SOURCES_WINDOWS below); one that has none is refused here, not left to fail as an undefined symbol.
if(NOT (CORE_CPP_PLATFORM_POSIX OR WIN32 OR EMSCRIPTEN))
    message(FATAL_ERROR "[core-cpp] no system random source for this platform: core::systemRandom() needs "
        "getentropy() (POSIX, Emscripten) or BCryptGenRandom() (Windows); add a source to the base module")
endif()

# BCryptGenRandom, behind core::systemRandom(); PRIVATE, no bcrypt type reaches a header.
set(_coreCppBaseWindowsLibs "")
if(WIN32)
    list(APPEND _coreCppBaseWindowsLibs bcrypt)
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
        Random.hpp
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
        Random.cpp
        SystemRandomSource.hpp
        UserInfo.cpp
        Utf8Path.cpp
        Utils.cpp
    # One fillFromSystemRandom() per platform (core::systemRandom()): getentropy() on every POSIX
    # system and under Emscripten, BCryptGenRandom() on Windows.
    SOURCES_POSIX
        PosixRandom.cpp
    SOURCES_EMSCRIPTEN
        PosixRandom.cpp
    # The conversions between UTF-16 and UTF-8 behind every wide Win32 call, core-cpp's own and its
    # consumers'.
    SOURCES_WINDOWS
        WideString.cpp
        WindowsRandom.cpp
    PRIVATE_LIBS ${_coreCppBaseWindowsLibs}
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
        Random_test.cpp
        Ranges_test.cpp
        Sha256_test.cpp
        Times_test.cpp
        Utf8Path_test.cpp
        Utils_test.cpp
    SOURCES_WINDOWS
        WideString_test.cpp)
