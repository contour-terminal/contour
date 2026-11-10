// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChildProcess.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <format>
#include <optional>
#include <thread>
#include <variant>

using namespace std::chrono_literals;
using vtpty::testing::execInfo;

#ifndef _WIN32
TEST_CASE("a Process is reachable as a ChildProcess", "[process][childprocess]")
{
    auto process =
        vtpty::testing::unplacedProcess(execInfo("/bin/sh", { "-c", "exit 7" }),
                                        vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
    REQUIRE(process.start().has_value());

    vtpty::Pty& pty = process;
    auto* const child = dynamic_cast<vtpty::ChildProcess*>(&pty);
    REQUIRE(child != nullptr);

    auto const crossing = child->crossing();
    CHECK(crossing.kind == vtpty::CrossingKind::Adoptable);
    CHECK(crossing.keeperSession == 0);
    REQUIRE(crossing.child.has_value());
    CHECK(crossing.child->pid > 0);

    auto status = std::optional<vtpty::ExitStatus> {};
    for (auto const deadline = std::chrono::steady_clock::now() + 2s;
         !status && std::chrono::steady_clock::now() < deadline;)
    {
        status = child->checkStatus();
        if (!status)
            std::this_thread::sleep_for(5ms);
    }
    REQUIRE(status.has_value());
    auto const* const normal = std::get_if<vtpty::NormalExit>(&*status);
    REQUIRE(normal != nullptr);
    CHECK(normal->exitCode == 7);
    CHECK(child->spawnProgress() == vtpty::SpawnProgress::Settled);
}
#endif

TEST_CASE("an ExitStatus formats as its kind and number", "[process][childprocess]")
{
    CHECK(std::format("{}", vtpty::ExitStatus { vtpty::NormalExit { 7 } }) == "7 (normal exit)");
#ifndef _WIN32
    auto const killed = std::format("{}", vtpty::ExitStatus { vtpty::SignalExit { SIGKILL } });
    CHECK(killed == "SIGKILL (signal number 9)");
    CHECK(std::format("{}", vtpty::ExitStatus { vtpty::SignalExit { 12345 } })
          == "unknown signal (signal number 12345)");
#endif
}
