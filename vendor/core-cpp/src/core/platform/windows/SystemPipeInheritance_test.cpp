// SPDX-License-Identifier: Apache-2.0
//
// The loopback socket pair behind a Windows `SystemPipe` must not be inherited by a child process
// (core-cpp#28). `::socket()` and `::accept()` hand back inheritable handles there, so a consumer
// that spawns a shell -- contour and endo both do -- handed the child the loop's wakeup channel,
// and a child that kept it open could hold that channel alive after the parent closed its end.
// POSIX sets `FD_CLOEXEC` and has no such case; this file is the Windows half.
#include <core/platform/SystemPipe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

TEST_CASE("A system pipe's handles are not inherited by a child process", "[SystemPipe][windows]")
{
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());
    for (auto const handle: { (*pipe)->readFd(), (*pipe)->writeFd(), (*pipe)->waitHandle() })
    {
        auto flags = DWORD {};
        REQUIRE(GetHandleInformation(static_cast<HANDLE>(handle), &flags) != 0);
        CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
    }
}
