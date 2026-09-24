// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Types.hpp>

// Captured before anything else is included: Types.hpp itself must not have brought in the
// Windows API, whose min/max macros and the rest would reach every consumer that includes it.
#ifdef _WINDOWS_
    #define CORE_CPP_TYPES_INCLUDED_WINDOWS_H 1
#else
    #define CORE_CPP_TYPES_INCLUDED_WINDOWS_H 0
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <string_view>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

using namespace core::platform;

TEST_CASE("Types.hpp does not include the Windows API", "[platform]")
{
    STATIC_CHECK(CORE_CPP_TYPES_INCLUDED_WINDOWS_H == 0);
}

TEST_CASE("InvalidHandle is the platform's own invalid-handle value", "[platform]")
{
#ifdef _WIN32
    CHECK(InvalidHandle == INVALID_HANDLE_VALUE);
#else
    STATIC_CHECK(InvalidHandle == -1);
#endif
    CHECK(nativeHandleToNumber(InvalidHandle) == -1);
}

TEST_CASE("the standard handles are the standard streams of the platform", "[platform]")
{
#ifndef _WIN32
    STATIC_CHECK(standardInput() == STDIN_FILENO);
    STATIC_CHECK(standardOutput() == STDOUT_FILENO);
    STATIC_CHECK(standardError() == STDERR_FILENO);
#endif
    CHECK(standardInput() != InvalidHandle);
    CHECK(standardOutput() != InvalidHandle);
    CHECK(standardError() != InvalidHandle);
}

TEST_CASE("platformRead.drained_pipe_whose_writer_closed", "[platform]")
{
    // Regression guard: on Windows, ReadFile on a drained pipe whose writer has
    // closed fails with ERROR_BROKEN_PIPE. platformRead must normalize that to
    // a POSIX-style EOF (return 0), not an I/O error (return -1).

#ifdef _WIN32
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    REQUIRE(CreatePipe(&readEnd, &writeEnd, nullptr, 0) != 0);
    auto const readH = static_cast<NativeHandle>(readEnd);

    auto const payload = std::string_view { "hi" };
    DWORD written = 0;
    REQUIRE(WriteFile(writeEnd, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) != 0);
    CHECK(written == payload.size());
    CloseHandle(writeEnd);
#else
    int fds[2] = { -1, -1 };
    REQUIRE(::pipe(fds) == 0);
    auto const readH = fds[0];
    auto const writeH = fds[1];

    auto const payload = std::string_view { "hi" };
    auto const expected = static_cast<intptr_t>(payload.size());
    auto const w = static_cast<intptr_t>(::write(writeH, payload.data(), payload.size()));
    REQUIRE(w == expected);
    ::close(writeH);
#endif

    std::array<char, 16> buf {};
    auto const first = platformRead(readH, buf.data(), buf.size());
    CHECK(first == 2);
    CHECK(std::string_view(buf.data(), static_cast<size_t>(first)) == "hi");

    // Second call: buffer empty, writer closed. Must be EOF, not error...
    errno = 0;
    auto const second = platformRead(readH, buf.data(), buf.size());
#ifdef __EMSCRIPTEN__
    // ...except under Emscripten, whose pipes have no end of file: a read of an empty pipe
    // fails with EAGAIN whether or not the writer has closed (see platformRead()).
    auto const readError = errno;
    CHECK(second == -1);
    CHECK(readError == EAGAIN);
#else
    CHECK(second == 0);
#endif

#ifdef _WIN32
    CloseHandle(readEnd);
#else
    ::close(readH);
#endif
}
