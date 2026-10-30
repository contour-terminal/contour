// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for exitCodeFor() — the pure policy behind ContourGuiApp::run()'s return value,
// extracted from the Qt event loop so the exit-status → exit-code mapping is testable in isolation.

#include <contour/session/ExitCode.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using contour::session::exitCodeFor;
using contour::session::QuickExitNotice;
using contour::session::quickExitNoticeFor;
using contour::session::SessionExitStatus;
using contour::session::sessionExitStatusOf;

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

TEST_CASE("quickExitNoticeFor: a quick exit is reported unless the shell said it succeeded",
          "[contour][exitcode]")
{
    // A shell that ends within early_exit_threshold may have failed to start -- its error is on screen
    // and must stay readable. A clean exit status says it did not: that is `exit` or Ctrl+D, and the
    // window closes as it would at any other time.
    using vtpty::Process;
    CHECK(quickExitNoticeFor(Process::ExitStatus { Process::NormalExit { 0 } }) == QuickExitNotice::Skip);
    CHECK(quickExitNoticeFor(Process::ExitStatus { Process::NormalExit { 1 } }) == QuickExitNotice::Show);
    CHECK(quickExitNoticeFor(Process::ExitStatus { Process::NormalExit { 127 } }) == QuickExitNotice::Show);
    CHECK(quickExitNoticeFor(Process::ExitStatus { Process::SignalExit { 11 } }) == QuickExitNotice::Show);
    // A device that cannot tell how its program ended is not taken to have succeeded.
    CHECK(quickExitNoticeFor(SessionExitStatus {}) == QuickExitNotice::Show);
}

#ifdef VTPTY_LIBSSH2
TEST_CASE("quickExitNoticeFor: an SSH session's remote exit status counts the same", "[contour][exitcode]")
{
    using vtpty::SshSession;
    CHECK(quickExitNoticeFor(SshSession::ExitStatus { SshSession::NormalExit { 0 } })
          == QuickExitNotice::Skip);
    CHECK(quickExitNoticeFor(SshSession::ExitStatus { SshSession::SignalExit { "TERM", "", "" } })
          == QuickExitNotice::Show);
}
#endif

TEST_CASE("sessionExitStatusOf: a device that runs no process reports no status", "[contour][exitcode]")
{
    auto const pty = vtpty::MockPty { vtpty::PageSize { vtpty::LineCount(5), vtpty::ColumnCount(20) } };
    CHECK_FALSE(sessionExitStatusOf(pty).has_value());
}

TEST_CASE("endOf: an exit names its code, a signal its number, and an unknown end nothing",
          "[contour][exitcode]")
{
    using contour::session::endOf;
    using contour::session::EndReason;
    using vtpty::Process;
    CHECK(endOf(Process::ExitStatus { Process::NormalExit { 1 } })
          == contour::session::SessionEndReport { .reason = EndReason::Exited, .detail = "1" });
    CHECK(endOf(Process::ExitStatus { Process::SignalExit { 9 } })
          == contour::session::SessionEndReport { .reason = EndReason::Signalled, .detail = "9" });
    // A device that cannot tell how its program ended (a mock, a mirror) still ended: it exited.
    CHECK(endOf(SessionExitStatus {}) == contour::session::SessionEndReport { .reason = EndReason::Exited });
}

#ifdef VTPTY_LIBSSH2
TEST_CASE("endOf: an SSH session's remote end counts the same", "[contour][exitcode]")
{
    using contour::session::endOf;
    using contour::session::EndReason;
    using vtpty::SshSession;
    CHECK(endOf(SshSession::ExitStatus { SshSession::NormalExit { 2 } })
          == contour::session::SessionEndReport { .reason = EndReason::Exited, .detail = "2" });
    CHECK(endOf(SshSession::ExitStatus { SshSession::SignalExit { "TERM", "", "" } })
          == contour::session::SessionEndReport { .reason = EndReason::Signalled, .detail = "TERM" });
}
#endif
