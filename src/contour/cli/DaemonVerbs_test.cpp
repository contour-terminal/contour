// SPDX-License-Identifier: Apache-2.0
//
// `contour daemon status | logs | stop`, driven over a fake control port, a fake prompt and an in-memory file
// system.

#include <contour/cli/DaemonVerbs.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/client/DaemonControlClient.hpp>

namespace
{

using vthost::client::ControlError;
using vthost::client::ControlErrorCode;
using vthost::client::DaemonStatus;

/// A control port answering what it was given.
class FakeControlPort final: public vthost::client::DaemonControlPort
{
  public:
    explicit FakeControlPort(DaemonStatus reply): _result { std::move(reply) } {}

    explicit FakeControlPort(ControlErrorCode code, std::string detail = {}):
        _result { std::unexpected(ControlError { .code = code, .detail = std::move(detail) }) }
    {
    }

    [[nodiscard]] std::expected<DaemonStatus, ControlError> status() override { return _result; }

    [[nodiscard]] std::expected<void, ControlError> stop(vthost::proto::StopReason reason) override
    {
        stops.push_back(reason);
        if (!_result)
            return std::unexpected(_result.error());
        return {};
    }

    std::vector<vthost::proto::StopReason> stops;

  private:
    std::expected<DaemonStatus, ControlError> _result;
};

/// A prompt giving one canned answer, remembering the question.
class FakePrompt final: public contour::cli::ConfirmPrompt
{
  public:
    explicit FakePrompt(contour::cli::Confirmation answer): _answer { answer } {}

    [[nodiscard]] contour::cli::Confirmation ask(std::string_view question) override
    {
        asked = question;
        return _answer;
    }

    std::string asked;

  private:
    contour::cli::Confirmation _answer;
};

DaemonStatus sampleStatus()
{
    return DaemonStatus { .releaseVersion = "0.8.1",
                          .processId = 4242,
                          .uptime = std::chrono::seconds { 7385 },
                          .socketPath = "/run/c/default",
                          .executablePath = "/usr/bin/contour",
                          .logFile = "/l.log",
                          .sessions = 3,
                          .clients = 1 };
}

} // namespace

TEST_CASE("uptime reads in at most three units", "[contour][cli][daemon]")
{
    using namespace std::chrono_literals;
    CHECK(contour::cli::formatUptime(45s) == "45s");
    CHECK(contour::cli::formatUptime(125s) == "2m 05s");
    CHECK(contour::cli::formatUptime(7385s) == "2h 03m 05s");
    CHECK(contour::cli::formatUptime(273600s) == "3d 04h 00m");
}

TEST_CASE("status names version, uptime, socket, sessions and clients", "[contour][cli][daemon]")
{
    auto port = FakeControlPort { sampleStatus() };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStatusVerb(port, "/run/c/default", out, err) == contour::cli::ExitStatus::Ok);
    for (auto const* needle:
         { "running (pid 4242)", "0.8.1", "2h 03m 05s", "/run/c/default", "sessions:", "3", "clients:" })
        CHECK(out.str().contains(needle));
}

TEST_CASE("status says standard error for a daemon with no log file", "[contour][cli][daemon]")
{
    auto status = sampleStatus();
    status.logFile.clear();
    CHECK(contour::cli::formatDaemonStatus(status).contains("log"));
    CHECK(contour::cli::formatDaemonStatus(status).contains("standard error"));
}

TEST_CASE("status of an absent daemon says so and exits 4 (unreachable)", "[contour][cli][daemon]")
{
    auto port = FakeControlPort { ControlErrorCode::NotRunning };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStatusVerb(port, "/run/c/default", out, err)
          == contour::cli::ExitStatus::Unreachable);
    CHECK(contour::cli::exitCode(contour::cli::ExitStatus::Unreachable) == 4);
    CHECK(out.str() == "no daemon is running on /run/c/default\n");
}

TEST_CASE("status of a failing daemon reports on standard error", "[contour][cli][daemon]")
{
    auto port = FakeControlPort { ControlErrorCode::Timeout };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStatusVerb(port, "/s", out, err) == contour::cli::ExitStatus::Failed);
    CHECK(out.str().empty());
    CHECK(!err.str().empty());
}

TEST_CASE("stop asks before ending sessions, and refuses without a terminal", "[contour][cli][daemon]")
{
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    SECTION("declined")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Declined };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Ask, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(port.stops.empty());
        CHECK(prompt.asked.contains("3 session"));
        CHECK(err.str().contains("not stopped"));
    }
    SECTION("no terminal")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Unavailable };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Ask, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(port.stops.empty());
        CHECK(err.str().contains("--yes"));
    }
    SECTION("confirmed")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Ask, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(port.stops.size() == 1);
    }
    SECTION("--yes")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Declined };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Given, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(port.stops == std::vector { vthost::proto::StopReason::User });
        CHECK(prompt.asked.empty());
    }
    SECTION("no sessions needs no consent")
    {
        auto status = sampleStatus();
        status.sessions = 0;
        auto port = FakeControlPort { status };
        auto prompt = FakePrompt { contour::cli::Confirmation::Declined };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Ask, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(prompt.asked.empty());
    }
    SECTION("no daemon")
    {
        auto port = FakeControlPort { ControlErrorCode::NotRunning };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Given, prompt, out, err)
              == contour::cli::ExitStatus::Unreachable);
        CHECK(err.str() == "no daemon is running on /run/c/default\n");
        CHECK(out.str().empty());
    }
    SECTION("a daemon without daemon control")
    {
        auto port = FakeControlPort { ControlErrorCode::Unsupported, "release 0.7.0" };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Given, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(err.str().contains("the daemon (release 0.7.0) does not support `contour daemon stop`"));
        CHECK(err.str().contains("Ctrl+C"));
    }
    SECTION("a daemon without daemon control that states no release")
    {
        auto port = FakeControlPort { ControlErrorCode::Unsupported, "an earlier release" };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", contour::cli::StopConsent::Given, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(
            err.str().starts_with("the daemon (an earlier release) does not support `contour daemon stop`"));
    }
}

TEST_CASE("a log follower prints what was appended and survives truncation", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/l.log", "a\nb\nc\n");
    auto follower = contour::cli::LogFollower { files, "/l.log" };
    CHECK(follower.tail(2) == "b\nc\n");
    std::ignore = files.appendFile("/l.log", "d\n");
    CHECK(follower.poll() == "d\n");
    CHECK(follower.poll().empty());
    std::ignore = files.writeFile("/l.log", "x\n");
    CHECK(follower.poll() == "x\n");
}

TEST_CASE("a log follower's tail of 0 lines is the whole file", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/l.log", "a\nb\nc\n");
    auto follower = contour::cli::LogFollower { files, "/l.log" };
    CHECK(follower.tail(0) == "a\nb\nc\n");
}

TEST_CASE("logs prints the running daemon's log, following until told to stop", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/l.log", "one\ntwo\n");
    auto port = FakeControlPort { sampleStatus() };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};

    SECTION("once")
    {
        auto const tick =
            std::function<contour::cli::FollowStep()> { [] { return contour::cli::FollowStep::Stop; } };
        CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", {}, files, tick, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(out.str() == "one\ntwo\n");
    }
    SECTION("follow")
    {
        auto ticks = 0;
        auto const tick = std::function<contour::cli::FollowStep()> { [&] {
            if (++ticks == 1)
            {
                std::ignore = files.appendFile("/l.log", "three\n");
                return contour::cli::FollowStep::Continue;
            }
            return contour::cli::FollowStep::Stop;
        } };
        auto const options =
            contour::cli::LogsOptions { .lines = 1, .follow = contour::cli::FollowMode::Follow };
        CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", options, files, tick, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(out.str() == "two\nthree\n");
    }
}

TEST_CASE("logs of an absent daemon reads the fallback file, noting it", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/fallback.log", "old\n");
    auto port = FakeControlPort { ControlErrorCode::NotRunning };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const tick =
        std::function<contour::cli::FollowStep()> { [] { return contour::cli::FollowStep::Stop; } };
    CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", {}, files, tick, out, err)
          == contour::cli::ExitStatus::Ok);
    CHECK(out.str() == "old\n");
    CHECK(err.str().contains("/fallback.log"));
}

TEST_CASE("logs of a daemon logging to standard error says so", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto status = sampleStatus();
    status.logFile.clear();
    auto port = FakeControlPort { status };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const tick =
        std::function<contour::cli::FollowStep()> { [] { return contour::cli::FollowStep::Stop; } };
    CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", {}, files, tick, out, err)
          == contour::cli::ExitStatus::Failed);
    CHECK(err.str().contains("the daemon logs to its standard error"));
}

TEST_CASE("options given before and after a sub-verb merge", "[contour][cli][daemon]")
{
    using contour::cli::mergeSharedOption;
    CHECK(mergeSharedOption("socket", "", "", "") == "");
    CHECK(mergeSharedOption("socket", "X", "", "") == "X");
    CHECK(mergeSharedOption("socket", "", "X", "") == "X");
    CHECK(mergeSharedOption("socket", "X", "X", "") == "X");
    auto const clash = mergeSharedOption("socket", "X", "Y", "");
    REQUIRE(!clash);
    CHECK(clash.error().contains("--socket"));
}
