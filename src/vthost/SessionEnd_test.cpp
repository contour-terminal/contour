// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vthost/SessionEnd.hpp>

using vthost::SessionEnd;
using vthost::sessionEndOf;
using vtpty::Process;

TEST_CASE("sessionEndOf tells a kill from an exit", "[vthost][snapshot]")
{
    CHECK(sessionEndOf(std::nullopt) == SessionEnd::Exited);
    CHECK(sessionEndOf(Process::ExitStatus { Process::NormalExit { 0 } }) == SessionEnd::Exited);
    CHECK(sessionEndOf(Process::ExitStatus { Process::NormalExit { 1 } }) == SessionEnd::Exited);
    constexpr auto Hangup = 1;
    constexpr auto Kill = 9;
    CHECK(sessionEndOf(Process::ExitStatus { Process::SignalExit { Hangup } }) == SessionEnd::Killed);
    CHECK(sessionEndOf(Process::ExitStatus { Process::SignalExit { Kill } }) == SessionEnd::Killed);
    CHECK(sessionEndOf(Process::ExitStatus { Process::NormalExit { vthost::WindowsControlCExitCode } })
          == SessionEnd::Killed);
}

TEST_CASE("every SessionEnd has a name", "[vthost][snapshot]")
{
    CHECK(describe(SessionEnd::Exited) == "exited");
    CHECK(describe(SessionEnd::Requested) == "closed on request");
    CHECK(describe(SessionEnd::Killed) == "killed");
}
