# SPDX-License-Identifier: Apache-2.0

# core::async: the coroutine vocabulary, header-only, needing nothing of the standard library but
# what a threaded standard library already links -- see Threads below.
# Task A5 imports contour's Task, cancellation and combinators; Task B1 adds fastcached's executors.
#
# Threads, on the same condition core::base uses, which is off exactly under single-threaded
# Emscripten (cmake/CoreCppDependencies.cmake). StopToken.hpp's fallback synchronises its stop
# state with a std::mutex, a std::condition_variable and std::this_thread::get_id() wherever
# CORE_CPP_USE_THREADS is on, and an INTERFACE target that declares none of that leaves
# `target_link_libraries(app PRIVATE core::async)` -- what the consumer guide shows, and what morph
# and tuidu will write -- failing to link wherever pthread is a library of its own and the fallback
# branch is taken. PUBLIC because the headers are all there is of this module.
set(_coreCppAsyncLibs "")
if(CORE_CPP_USE_THREADS)
    list(APPEND _coreCppAsyncLibs Threads::Threads)
endif()

# ThreadPoolExecutor.hpp is the module's one platform-shaped header: it needs threads, and
# single-threaded Emscripten has none, so it refuses to compile there (its own #error) and is in no
# file set that build installs. Everything else is in the WebAssembly subset.
#
# It reaches HEADERS through a variable of its own rather than a list(APPEND) over the whole list,
# so the rest stays literal where the file set is declared and a reader sees everything this module
# publishes in one place. tools/migrate/check-renames.py reads this call to answer whether a header
# a rename row names is one a consumer can include; it follows a set() and a list(APPEND) alike, and
# a ${...} it cannot resolve in full it reports, rather than reading the module as publishing less.
set(_coreCppAsyncThreadedHeaders "")
if(CORE_CPP_USE_THREADS)
    set(_coreCppAsyncThreadedHeaders ThreadPoolExecutor.hpp)
endif()

core_cpp_add_module(async KIND INTERFACE
    HEADERS
        AsTask.hpp
        AsyncQueue.hpp
        Awaitable.hpp
        Cancellation.hpp
        DetachedTask.hpp
        ExecutorContext.hpp
        IExecutor.hpp
        Join.hpp
        KeyedStrands.hpp
        ParkedWork.hpp
        ResumeOn.hpp
        StopToken.hpp
        Strand.hpp
        SyncRun.hpp
        Task.hpp
        UniqueCoroHandle.hpp
        WhenAll.hpp
        WhenAny.hpp
        testing/ManualExecutor.hpp
        ${_coreCppAsyncThreadedHeaders}
    PUBLIC_LIBS ${_coreCppAsyncLibs})

# That the module carries the dependency, asserted here because a host whose pthread is part of
# libc (glibc 2.34 and later, the Windows CRT, libSystem) links the program below either way and
# would report nothing.
get_target_property(_coreCppAsyncInterfaceLibs core-cpp-async INTERFACE_LINK_LIBRARIES)
if(CORE_CPP_USE_THREADS AND NOT "${_coreCppAsyncInterfaceLibs}" MATCHES "Threads::Threads")
    message(FATAL_ERROR
        "[core-cpp] core::async does not link Threads, although <core/async/StopToken.hpp>'s "
        "fallback uses std::mutex, std::condition_variable and std::this_thread. A consumer that "
        "links core::async alone would fail to link wherever pthread is a library of its own.")
endif()

# The tests of everything that names a StopToken -- which is everything that includes Task.hpp,
# whose promise holds one. Both binaries below build this list, so a case added here is run over
# std::stop_token and over the fallback.
set(_coreCppAsyncStopTokenTests
    AsTask_test.cpp
    AsyncQueue_test.cpp
    Awaitable_test.cpp
    KeyedStrands_test.cpp
    ParkedWork_test.cpp
    StopToken_test.cpp
    Strand_test.cpp
    Task_test.cpp
    WhenAll_test.cpp
    WhenAny_test.cpp)
# ThreadPoolExecutor is the one part of the module a single-threaded WebAssembly build does not
# have, so its test is not compiled there either.
if(CORE_CPP_USE_THREADS)
    list(APPEND _coreCppAsyncStopTokenTests ThreadPoolExecutor_test.cpp)
endif()

# Whether this build's GCC makes the symmetric transfer of a deep co_await chain a tail call. It
# does so only when it optimises sibling calls, from -O2 up, and it defines no macro for the level:
# __OPTIMIZE__ is 1 at -Og and -O1 too, where the 100000-frame chain of Task_test.cpp's deep-chain
# case overflows the stack and takes every other case in the binary with it. So the build says, and
# the case skips wherever the answer is not yes -- including every build outside core-cpp's presets
# (core-cpp#15). Clang and MSVC make the transfer a tail call at every level and are not asked.
set(_coreCppAsyncDefinitions "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_coreCppAsyncOptimisation "${CMAKE_CXX_FLAGS}")
    if(CMAKE_BUILD_TYPE)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _coreCppAsyncConfiguration)
        string(APPEND _coreCppAsyncOptimisation " ${CMAKE_CXX_FLAGS_${_coreCppAsyncConfiguration}}")
    endif()
    # The last -O on the command line is the one that counts, as it is for the compiler; GCC's
    # default, where there is none at all, is -O0.
    set(_coreCppAsyncLevel "-O0")
    string(REGEX MATCHALL "(^| )-O[0-9A-Za-z]*" _coreCppAsyncLevels "${_coreCppAsyncOptimisation}")
    if(_coreCppAsyncLevels)
        list(POP_BACK _coreCppAsyncLevels _coreCppAsyncLevel)
        string(STRIP "${_coreCppAsyncLevel}" _coreCppAsyncLevel)
    endif()
    set(_coreCppAsyncTailCall "no, so the deep-chain case is skipped")
    if(_coreCppAsyncLevel MATCHES "^-O([23]|fast)$")
        list(APPEND _coreCppAsyncDefinitions CORE_ASYNC_SYMMETRIC_TRANSFER_IS_TAIL_CALL=1)
        set(_coreCppAsyncTailCall "yes")
    endif()
    message(STATUS "[core-cpp] async: GCC at ${_coreCppAsyncLevel}, symmetric transfer tail-calls: "
                   "${_coreCppAsyncTailCall}")
endif()

core_cpp_add_test(async
    SOURCES ${_coreCppAsyncStopTokenTests}
    DEFINITIONS ${_coreCppAsyncDefinitions})

# The same tests over the StopToken fallback, on every platform, whether or not the standard
# library has std::stop_token. A binary of their own: CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK changes
# what StopToken is, so translation units with and without it in one program would give every
# inline function that names a StopToken two definitions.
core_cpp_add_test(async NAME async-fallback
    SOURCES ${_coreCppAsyncStopTokenTests}
    DEFINITIONS CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK ${_coreCppAsyncDefinitions})

# What a strand does when an allocation it needs fails: a binary of its own because it replaces the
# global allocation functions, which would reach every case linked beside it. Not under a sanitizer,
# whose runtime defines those functions itself (the link refuses a second definition).
if(NOT CORE_CPP_SANITIZERS)
    core_cpp_add_test(async NAME async-alloc SOURCES StrandAllocation_test.cpp ../testing/ReplacedGlobalAllocation.cpp)
    # The same reason, and a replacement of its own: freed coroutine frames become inaccessible
    # pages, so a ramp that reads its frame after the coroutine finished faults (core-cpp#51).
    core_cpp_add_test(async NAME async-detached-frame
        SOURCES DetachedTaskFrame_test.cpp ../testing/ReplacedGlobalAllocation.cpp)
endif()

# What a strand does with a task that throws out of resume(), which differs by compiler: under MSVC's
# cl it ends the process with a message (Strand.hpp), everywhere else the exception reaches the base
# and the strand goes on. A process of its own because one branch ends it; registered with the
# branch this compiler takes as the PASS and the other as the FAIL, so neither can pass for the other.
if(CORE_CPP_TESTING AND NOT EMSCRIPTEN)
    add_executable(core-cpp-strand-throw-canary StrandThrowCanary.cpp)
    # core::testing_dialogs, because an abort in an MSVC Debug build opens a message box and waits.
    target_link_libraries(core-cpp-strand-throw-canary PRIVATE core::async core::testing_dialogs)
    core_cpp_apply_toolchain(core-cpp-strand-throw-canary)
    add_test(NAME core-cpp.strand-throw-canary COMMAND core-cpp-strand-throw-canary)
    set(_coreCppStrandTerminates "core::async::Strand: a task threw out of resume[(][)]")
    set(_coreCppStrandPropagates "strand-throw-canary: the task behind it ran")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set_tests_properties(core-cpp.strand-throw-canary PROPERTIES
            PASS_REGULAR_EXPRESSION "${_coreCppStrandTerminates}"
            FAIL_REGULAR_EXPRESSION "strand-throw-canary: the throw reached the base")
    else()
        set_tests_properties(core-cpp.strand-throw-canary PROPERTIES
            PASS_REGULAR_EXPRESSION "${_coreCppStrandPropagates}"
            FAIL_REGULAR_EXPRESSION "${_coreCppStrandTerminates}")
    endif()
    set_tests_properties(core-cpp.strand-throw-canary PROPERTIES LABELS "core-cpp;async;canary" TIMEOUT 60)
endif()

# The link a consumer makes: core::async alone, with the fallback forced, so that what the
# fallback needs has to come from core::async's own usage requirements. It is not a
# core_cpp_add_test(), because that links core::testing_main, which brings core::base, which links
# Threads -- which is exactly what hid this.
if(CORE_CPP_TESTING)
    add_executable(core-cpp-async-link-smoke StopTokenLinkSmoke.cpp)
    target_link_libraries(core-cpp-async-link-smoke PRIVATE core::async)
    target_compile_definitions(core-cpp-async-link-smoke PRIVATE CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK)
    core_cpp_apply_toolchain(core-cpp-async-link-smoke)
    add_test(NAME core-cpp.async-link-smoke COMMAND core-cpp-async-link-smoke)
    # Bounded here because core_cpp_add_test()'s 300-second default does not reach a bare
    # add_test(), and an unbounded one inherits ctest's 1500 seconds, which names neither the test
    # nor what it waited for. 60 rather than 300: this program asserts a handful of StopToken
    # operations and returns, so it runs in milliseconds or it is broken, and a bound that says so
    # is worth more than one copied from a suite whose slowest binary takes nine seconds.
    set_tests_properties(core-cpp.async-link-smoke PROPERTIES LABELS "core-cpp;async;hygiene" TIMEOUT 60)
endif()

# Which StopToken this toolchain gets, in the configure log: whether core-cpp.async tests
# std::stop_token or the fallback differs between CI jobs, and nothing else shows it. Each probe
# compiles StopToken.hpp itself and refuses one answer, so exactly one compiles; neither compiling
# means the probe is broken, which is said rather than read as either answer.
if(CORE_CPP_TESTING)
    set(_coreCppStopTokenAnswers "")
    foreach(_coreCppStopTokenProbe IN ITEMS "std::stop_token|!CORE_ASYNC_STOP_TOKEN_IS_STD"
                                            "core-cpp's fallback|CORE_ASYNC_STOP_TOKEN_IS_STD")
        string(REPLACE "|" ";" _coreCppStopTokenProbe "${_coreCppStopTokenProbe}")
        list(GET _coreCppStopTokenProbe 0 _coreCppStopTokenAnswer)
        list(GET _coreCppStopTokenProbe 1 _coreCppStopTokenRefusal)
        try_compile(CORE_CPP_ASYNC_STOP_TOKEN_PROBE
            SOURCE_FROM_CONTENT core-cpp-stop-token-probe.cpp
                "#include <core/async/StopToken.hpp>\n#if ${_coreCppStopTokenRefusal}\n#error refused\n#endif\nint main() { return 0; }\n"
            CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CORE_CPP_SOURCE_DIR}/src"
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            NO_CACHE)
        if(CORE_CPP_ASYNC_STOP_TOKEN_PROBE)
            list(APPEND _coreCppStopTokenAnswers "${_coreCppStopTokenAnswer}")
        endif()
    endforeach()
    list(LENGTH _coreCppStopTokenAnswers _coreCppStopTokenAnswerCount)
    if(_coreCppStopTokenAnswerCount EQUAL 1)
        message(STATUS "[core-cpp] async: StopToken is ${_coreCppStopTokenAnswers}")
    else()
        message(WARNING "[core-cpp] async: could not tell which StopToken this toolchain gets "
                        "(probes that compiled: '${_coreCppStopTokenAnswers}')")
    endif()
endif()
