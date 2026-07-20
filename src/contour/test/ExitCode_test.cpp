// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for exitCodeFor() — the pure policy behind ContourGuiApp::run()'s return value,
// extracted from the Qt event loop so the exit-status → exit-code mapping is testable in isolation.

#include <contour/ExitCode.h>

#include <vtpty/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using contour::exitCodeFor;
using contour::SessionExitStatus;

TEST_CASE("exitCodeFor: no session status preserves the event-loop fallback", "[contour][exitcode]")
{
    CHECK(exitCodeFor(SessionExitStatus {}, 0) == 0);
    CHECK(exitCodeFor(SessionExitStatus {}, 42) == 42);
}

TEST_CASE("exitCodeFor: a process normal exit propagates the child's exit code", "[contour][exitcode]")
{
    SessionExitStatus const status = vtpty::Process::ExitStatus { vtpty::Process::NormalExit { 7 } };
    // The child's code wins over the loop fallback.
    CHECK(exitCodeFor(status, 0) == 7);

    SessionExitStatus const zero = vtpty::Process::ExitStatus { vtpty::Process::NormalExit { 0 } };
    CHECK(exitCodeFor(zero, 99) == 0);
}

TEST_CASE("exitCodeFor: a process signal exit maps to EXIT_FAILURE", "[contour][exitcode]")
{
    SessionExitStatus const status = vtpty::Process::ExitStatus { vtpty::Process::SignalExit { 9 } };
    CHECK(exitCodeFor(status, 0) == EXIT_FAILURE);
}

#ifdef VTPTY_LIBSSH2
TEST_CASE("exitCodeFor: an SSH normal exit propagates its code and a signal exit fails",
          "[contour][exitcode]")
{
    SessionExitStatus const normal = vtpty::SshSession::ExitStatus { vtpty::SshSession::NormalExit { 3 } };
    CHECK(exitCodeFor(normal, 0) == 3);

    SessionExitStatus const signalled =
        vtpty::SshSession::ExitStatus { vtpty::SshSession::SignalExit { "TERM", "", "" } };
    CHECK(exitCodeFor(signalled, 0) == EXIT_FAILURE);
}
#endif
