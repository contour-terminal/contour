// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandCapture.hpp>
#include <vtpty/SshTerminfoInstall.hpp>

#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>

using namespace std::string_view_literals;
using namespace vtpty;

namespace
{
constexpr auto TestSource = "contour-test|test entry,\n\tam, cols#80,\n"sv;

std::string commandFor(std::string_view source = TestSource, std::string_view name = "contour-test")
{
    auto command = buildTerminfoInstallCommand(name, source);
    REQUIRE(command.has_value());
    return *command;
}
} // namespace

TEST_CASE("install command is one line of sh -c with a single-quoted body", "[ssh][terminfo]")
{
    auto const command = commandFor();
    CHECK(command.starts_with("sh -c '"));
    CHECK(command.ends_with("'"));
    CHECK_FALSE(command.contains('\n'));
}

TEST_CASE("install command body is shell-neutral", "[ssh][terminfo]")
{
    auto const command = commandFor("a'b\\c`d|e$f%g\nh,\n"sv);
    auto const body = std::string_view { command }.substr(7, command.size() - 8);
    CHECK_FALSE(body.contains('\''));
    CHECK_FALSE(body.contains('\\'));
}

TEST_CASE("invalid inputs are refused", "[ssh][terminfo]")
{
    auto const refusal = [](std::string_view name, std::string_view source) {
        auto const command = buildTerminfoInstallCommand(name, source);
        REQUIRE_FALSE(command.has_value());
        return command.error();
    };
    CHECK(refusal("contour", "\xC3\xA9"sv) == TerminfoCommandError::NonAsciiSource);
    CHECK(refusal("contour", ""sv) == TerminfoCommandError::EmptySource);
    CHECK(refusal("Con tour", "x,"sv) == TerminfoCommandError::InvalidEntryName);
    CHECK(refusal("", "x,"sv) == TerminfoCommandError::InvalidEntryName);
    CHECK(refusal("contour", std::string(20000, 'a')) == TerminfoCommandError::TooLong);
}

TEST_CASE("every command error and outcome has a name", "[ssh][terminfo]")
{
    for (auto const error: { TerminfoCommandError::EmptySource,
                             TerminfoCommandError::NonAsciiSource,
                             TerminfoCommandError::InvalidEntryName,
                             TerminfoCommandError::TooLong })
        CHECK_FALSE(describeTerminfoCommandError(error).empty());
    CHECK(terminfoOutcomeName(TerminfoInstallOutcome::Installed) == "installed");
    CHECK(terminfoOutcomeName(TerminfoInstallOutcome::Failed) == "failed");
    CHECK(terminfoOutcomeName(TerminfoInstallOutcome::TimedOut) == "timed out");
    CHECK(terminfoOutcomeName(TerminfoInstallOutcome::NotAttempted) == "not attempted");
}

TEST_CASE("a budget that ends after the marker still counts as installed", "[ssh][terminfo]")
{
    CHECK(judgeExpiredTerminfoRead("junk\nCONTOUR-TERMINFO-INSTALLED\n"sv)
          == TerminfoInstallOutcome::Installed);
    CHECK(judgeExpiredTerminfoRead("CONTOUR-TERMINFO-INSTALLED"sv) == TerminfoInstallOutcome::Installed);
    CHECK(judgeExpiredTerminfoRead(""sv) == TerminfoInstallOutcome::TimedOut);
    CHECK(judgeExpiredTerminfoRead("partial output"sv) == TerminfoInstallOutcome::TimedOut);
}

TEST_CASE("a read that fails after the marker still counts as installed", "[ssh][terminfo]")
{
    using enum TerminfoInstallOutcome;
    struct Row
    {
        std::string_view output;
        SshStepFailure failure {};
        TerminfoInstallOutcome expected {};
    };
    // The marker first: libssh2's timeout can fire, or the read fail, after the host already printed it.
    auto const table = std::array {
        Row { "CONTOUR-TERMINFO-INSTALLED\n"sv, SshStepFailure::Failed, Installed },
        Row { "CONTOUR-TERMINFO-INSTALLED\n"sv, SshStepFailure::TimedOut, Installed },
        Row { "partial output"sv, SshStepFailure::Failed, Failed },
        Row { "partial output"sv, SshStepFailure::TimedOut, TimedOut },
        Row { ""sv, SshStepFailure::Failed, Failed },
        Row { ""sv, SshStepFailure::TimedOut, TimedOut },
    };
    for (auto const& row: table)
    {
        INFO("output \"" << row.output << "\" failure " << static_cast<int>(row.failure));
        CHECK(judgeFailedTerminfoRead(row.output, row.failure) == row.expected);
    }
}

TEST_CASE("judge requires the marker line", "[ssh][terminfo]")
{
    CHECK(judgeTerminfoInstallOutput("") == TerminfoInstallOutcome::Failed);
    CHECK(judgeTerminfoInstallOutput("CONTOUR-TERMINFO-INSTALLED\n") == TerminfoInstallOutcome::Installed);
    CHECK(judgeTerminfoInstallOutput("CONTOUR-TERMINFO-INSTALLED\r\n") == TerminfoInstallOutcome::Installed);
    CHECK(judgeTerminfoInstallOutput("xCONTOUR-TERMINFO-INSTALLED\n") == TerminfoInstallOutcome::Failed);
    CHECK(judgeTerminfoInstallOutput("noise\nCONTOUR-TERMINFO-INSTALLED\n")
          == TerminfoInstallOutcome::Installed);
}

TEST_CASE("chooseSshTerm", "[ssh][terminfo]")
{
    CHECK(chooseSshTerm(TerminfoInstallOutcome::Installed, "contour", "xterm-256color") == "contour");
    CHECK(chooseSshTerm(TerminfoInstallOutcome::NotAttempted, "contour", "xterm-256color")
          == "xterm-256color");
    CHECK(chooseSshTerm(TerminfoInstallOutcome::Failed, "contour", "xterm-256color") == "xterm-256color");
    CHECK(chooseSshTerm(TerminfoInstallOutcome::TimedOut, "contour", "xterm-256color") == "xterm-256color");
    CHECK(chooseSshTerm(TerminfoInstallOutcome::Failed, "contour", "").empty());
}

TEST_CASE("effectiveTerminfoInstall applies the pinned-TERM rule", "[ssh][terminfo]")
{
    CHECK(effectiveTerminfoInstall(TerminfoInstall::Install, std::nullopt, "contour")
          == TerminfoInstall::Install);
    CHECK(effectiveTerminfoInstall(TerminfoInstall::Install, ""sv, "contour") == TerminfoInstall::Install);
    CHECK(effectiveTerminfoInstall(TerminfoInstall::Install, "contour"sv, "contour")
          == TerminfoInstall::Install);
    CHECK(effectiveTerminfoInstall(TerminfoInstall::Install, "xterm-256color"sv, "contour")
          == TerminfoInstall::Off);
    CHECK(effectiveTerminfoInstall(TerminfoInstall::Off, std::nullopt, "contour") == TerminfoInstall::Off);
}

TEST_CASE("the install budget never runs past the connection's", "[ssh][terminfo]")
{
    using namespace std::chrono;
    auto const now = steady_clock::now();
    CHECK(terminfoInstallDeadline(now, std::nullopt) == now + TerminfoInstallTimeout);
    CHECK(terminfoInstallDeadline(now, now + seconds { 30 }) == now + TerminfoInstallTimeout);
    CHECK(terminfoInstallDeadline(now, now + milliseconds { 1200 }) == now + milliseconds { 1200 });
    CHECK(terminfoInstallDeadline(now, now) == now);
}

TEST_CASE("appendTail keeps the last bytes", "[ssh][terminfo]")
{
    auto buffer = std::string {};
    appendTail(buffer, "0123456789", 8);
    CHECK(buffer == "23456789");

    buffer.clear();
    appendTail(buffer, "abcde", 8);
    appendTail(buffer, "fghij", 8);
    CHECK(buffer == "cdefghij");
}

TEST_CASE("host memo remembers failures per key", "[ssh][terminfo]")
{
    CHECK(terminfoHostKey("me", "h", 22) == "me@h:22");
    auto memo = InMemorySshTerminfoHostMemo {};
    auto const key = terminfoHostKey("me", "h", 22);
    CHECK_FALSE(memo.hasFailed(key));
    memo.recordFailure(key);
    CHECK(memo.hasFailed(key));
    CHECK_FALSE(memo.hasFailed(terminfoHostKey("me", "h", 2222)));
}

#ifndef _WIN32
namespace
{
/// The absolute path of @p program on PATH, or empty when it is not installed.
std::string findOnPath(std::string const& program)
{
    auto capture = SystemCommandCapture { "/bin/sh", {} };
    return capture.run("command -v " + program, std::chrono::seconds { 5 }, {}).value_or(std::string {});
}

std::string runUnder(std::string const& shell, std::string const& command)
{
    auto capture = SystemCommandCapture { shell, {} };
    auto const result = capture.run(command, std::chrono::seconds { 20 }, {});
    REQUIRE(result.has_value());
    return *result;
}

} // namespace

TEST_CASE("install command runs under a local shell", "[ssh][terminfo]")
{
    if (findOnPath("tic").empty())
        SKIP("tic is not on PATH");

    auto const tmp = core::testing::ScopedTempDir { "terminfo_install" };
    auto const home = tmp.path() / "home dir";
    auto const tmpdir = tmp.path() / "tmp dir";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(tmpdir);
    auto const homeEnv = core::testing::ScopedEnv { "HOME", home.string() };
    auto const tmpEnv = core::testing::ScopedEnv { "TMPDIR", tmpdir.string() };
    auto const command = commandFor();

    auto const installed = [&] {
        auto const hex = home / ".terminfo" / "63" / "contour-test";
        auto const letter = home / ".terminfo" / "c" / "contour-test";
        return std::filesystem::exists(hex) != std::filesystem::exists(letter);
    };
    auto const noLeftover = [&] {
        auto found = false;
        if (!std::filesystem::is_directory(home / ".terminfo"))
            return true;
        for (auto const& entry: std::filesystem::recursive_directory_iterator(home / ".terminfo"))
            found = found || entry.path().filename().string().starts_with(".contour-test.");
        return !found;
    };

    auto const tmpdirIsEmpty = [&] {
        return std::filesystem::is_empty(tmpdir);
    };

    SECTION("installs")
    {
        CHECK(judgeTerminfoInstallOutput(runUnder("/bin/sh", command)) == TerminfoInstallOutcome::Installed);
        CHECK(installed());
        CHECK(noLeftover());
        CHECK(tmpdirIsEmpty()); // the EXIT trap removed the scratch directory
    }

    SECTION("reinstall overwrites")
    {
        CHECK(judgeTerminfoInstallOutput(runUnder("/bin/sh", command)) == TerminfoInstallOutcome::Installed);
        CHECK(judgeTerminfoInstallOutput(runUnder("/bin/sh", command)) == TerminfoInstallOutcome::Installed);
        CHECK(installed());
    }

    SECTION("unwritable destination")
    {
        {
            auto file = std::ofstream { home / ".terminfo" };
            file << "not a directory";
        }
        CHECK(judgeTerminfoInstallOutput(runUnder("/bin/sh", command)) == TerminfoInstallOutcome::Failed);
        CHECK(tmpdirIsEmpty());
        auto content = std::string {};
        std::ifstream { home / ".terminfo" } >> content;
        CHECK(content == "not");
        CHECK(std::filesystem::is_regular_file(home / ".terminfo"));
    }

    for (auto const* shell: { "sh", "bash", "dash", "zsh", "fish", "csh" })
    {
        DYNAMIC_SECTION("installs under " << shell)
        {
            auto const path = findOnPath(shell);
            if (path.empty())
                SKIP(shell << " is not on PATH");
            CHECK(judgeTerminfoInstallOutput(runUnder(path, command)) == TerminfoInstallOutcome::Installed);
            CHECK(installed());
            CHECK(noLeftover());
            CHECK(tmpdirIsEmpty());
        }
    }
}
#endif

TEST_CASE("a timed-out open, a pending packet or a spent connect budget ends the session after the install",
          "[ssh][terminfo]")
{
    using enum TerminfoInstallOutcome;
    using enum TerminfoInstallStage;
    using enum SshOutbound;
    using enum AfterTerminfoInstall;
    struct Row
    {
        TerminfoInstallStage stage {};
        TerminfoInstallOutcome outcome {};
        SshOutbound outbound {};
        SshBudget connectBudget {};
        AfterTerminfoInstall expected {};
    };
    // Every combination, spelled out: a pending packet ends the session whatever the outcome (libssh2's
    // timeout can fire before ours and leave Failed or Installed), and so does a timed-out open; otherwise a
    // spent connect budget ends it as a connect timeout, even after a successful install.
    auto const table = std::array {
        Row { OpenChannel, Installed, Idle, SshBudget::Left, StartShell },
        Row { OpenChannel, Installed, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { OpenChannel, Installed, Pending, SshBudget::Left, EndSession },
        Row { OpenChannel, Installed, Pending, SshBudget::Spent, EndSession },
        Row { OpenChannel, Failed, Idle, SshBudget::Left, StartShell },
        Row { OpenChannel, Failed, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { OpenChannel, Failed, Pending, SshBudget::Left, EndSession },
        Row { OpenChannel, Failed, Pending, SshBudget::Spent, EndSession },
        Row { OpenChannel, TimedOut, Idle, SshBudget::Left, EndSession },
        Row { OpenChannel, TimedOut, Idle, SshBudget::Spent, EndSession },
        Row { OpenChannel, TimedOut, Pending, SshBudget::Left, EndSession },
        Row { OpenChannel, TimedOut, Pending, SshBudget::Spent, EndSession },
        Row { Exec, Installed, Idle, SshBudget::Left, StartShell },
        Row { Exec, Installed, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { Exec, Installed, Pending, SshBudget::Left, EndSession },
        Row { Exec, Installed, Pending, SshBudget::Spent, EndSession },
        Row { Exec, Failed, Idle, SshBudget::Left, StartShell },
        Row { Exec, Failed, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { Exec, Failed, Pending, SshBudget::Left, EndSession },
        Row { Exec, Failed, Pending, SshBudget::Spent, EndSession },
        Row { Exec, TimedOut, Idle, SshBudget::Left, StartShell },
        Row { Exec, TimedOut, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { Exec, TimedOut, Pending, SshBudget::Left, EndSession },
        Row { Exec, TimedOut, Pending, SshBudget::Spent, EndSession },
        Row { Read, Installed, Idle, SshBudget::Left, StartShell },
        Row { Read, Installed, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { Read, Installed, Pending, SshBudget::Left, EndSession },
        Row { Read, Installed, Pending, SshBudget::Spent, EndSession },
        Row { Read, Failed, Idle, SshBudget::Left, StartShell },
        Row { Read, Failed, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { Read, Failed, Pending, SshBudget::Left, EndSession },
        Row { Read, Failed, Pending, SshBudget::Spent, EndSession },
        Row { Read, TimedOut, Idle, SshBudget::Left, StartShell },
        Row { Read, TimedOut, Idle, SshBudget::Spent, ConnectTimedOut },
        Row { Read, TimedOut, Pending, SshBudget::Left, EndSession },
        Row { Read, TimedOut, Pending, SshBudget::Spent, EndSession },
    };
    for (auto const& row: table)
    {
        INFO("stage " << static_cast<int>(row.stage) << " outcome " << static_cast<int>(row.outcome)
                      << " outbound " << static_cast<int>(row.outbound) << " budget "
                      << static_cast<int>(row.connectBudget));
        CHECK(afterTerminfoInstall(row.stage, row.outcome, row.outbound, row.connectBudget) == row.expected);
    }
}

TEST_CASE("a pending packet tears the session down without blocking", "[ssh][terminfo]")
{
    STATIC_CHECK(sshTeardownFor(SshOutbound::Idle) == SshTeardown::AsConfigured);
    STATIC_CHECK(sshTeardownFor(SshOutbound::Pending) == SshTeardown::NonBlocking);
}
