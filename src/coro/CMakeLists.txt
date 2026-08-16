add_library(coro INTERFACE
    Awaitable.hpp
    Cancellation.hpp
    Task.hpp
    UniqueCoroHandle.hpp
    WhenAll.hpp
    WhenAny.hpp
    testing/SuppressWindowsDialogs.hpp
)
target_include_directories(coro INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

# Cancellation.hpp is built on C++20 <stop_token>, and refuses to compile without it (#error). One
# standard library we target does ship it but keeps it switched off: libc++ guards stop_token and
# jthread with _LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN, defined unless _LIBCPP_ENABLE_EXPERIMENTAL is
# -- which is what -fexperimental-library sets. libc++ 20 dropped the guard, so this affects
# libc++ 18 and 19 only; FreeBSD 15's base toolchain (Clang 19.1.7) is the one CI meets it on.
#
# Probed, not version-tested: what matters is the standard library the compiler actually uses, which
# its version number does not name -- and a probe stops adding the flag by itself once a platform
# moves on to libc++ 20, instead of pinning a workaround to a version range that has to be curated.
include(CheckCXXSourceCompiles)
set(_coro_stop_token_probe "
#include <version>
#if !defined(__cpp_lib_jthread) || __cpp_lib_jthread < 201911L
#error stop_token unavailable
#endif
int main() { return 0; }
")
check_cxx_source_compiles("${_coro_stop_token_probe}" CORO_HAS_STOP_TOKEN)
if(NOT CORO_HAS_STOP_TOKEN)
    set(CMAKE_REQUIRED_FLAGS "-fexperimental-library")
    check_cxx_source_compiles("${_coro_stop_token_probe}" CORO_HAS_STOP_TOKEN_WHEN_EXPERIMENTAL)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT CORO_HAS_STOP_TOKEN_WHEN_EXPERIMENTAL)
        message(FATAL_ERROR
            "coro requires C++20 <stop_token> (__cpp_lib_jthread), which this standard library does "
            "not provide even with -fexperimental-library. Use libstdc++ 11+, libc++ 18+, or MSVC "
            "STL -- or add the minimal fallback that src/coro/Cancellation.hpp describes.")
    endif()
    # Compile AND link: -fexperimental-library also selects the matching libc++experimental.
    target_compile_options(coro INTERFACE -fexperimental-library)
    target_link_options(coro INTERFACE -fexperimental-library)
    message(STATUS "[coro] <stop_token>: enabling -fexperimental-library (libc++ keeps it gated)")
endif()

option(CORO_TESTING "Enables building of unittests for coro [default: ON]" ${CONTOUR_TESTING})
if(CORO_TESTING)
    enable_testing()
    add_executable(coro_test
        test_main.cpp
        Task_test.cpp
        WhenAll_test.cpp
        WhenAny_test.cpp
    )
    target_link_libraries(coro_test coro Catch2::Catch2)
    add_test(NAME coro_test COMMAND $<TARGET_FILE:coro_test>)
    set_tests_properties(coro_test PROPERTIES LABELS daemon)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(coro_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
