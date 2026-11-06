// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/StateCodec.hpp>

#include <vtpty/CommandCapture.hpp>

#include <core/Environment.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vthost/handover/HandoverImage.hpp>
#include <vthost/handover/Readiness.hpp>

using namespace vthost::handover;
using namespace std::chrono_literals;

namespace
{

/// A command capture that answers as told, and records what it was asked to run.
class FakeCapture final: public vtpty::CommandCapture
{
  public:
    explicit FakeCapture(std::expected<std::string, vtpty::CommandCaptureError> answer):
        _answer { std::move(answer) }
    {
    }

    [[nodiscard]] std::expected<std::string, vtpty::CommandCaptureError> run(
        std::string_view command, std::chrono::milliseconds timeout, core::async::StopToken) override
    {
        commands.emplace_back(command);
        timeouts.push_back(timeout);
        return _answer;
    }

    std::vector<std::string> commands;
    std::vector<std::chrono::milliseconds> timeouts;

  private:
    std::expected<std::string, vtpty::CommandCaptureError> _answer;
};

/// A command capture that answers only once released, and says on which thread it ran.
class GatedCapture final: public vtpty::CommandCapture
{
  public:
    explicit GatedCapture(std::string answer): _answer { std::move(answer) } {}

    [[nodiscard]] std::expected<std::string, vtpty::CommandCaptureError> run(
        std::string_view /*command*/, std::chrono::milliseconds /*timeout*/, core::async::StopToken) override
    {
        ranOn = std::this_thread::get_id();
        _released.acquire();
        return _answer;
    }

    /// Lets run() answer.
    void release() { _released.release(); }

    std::thread::id ranOn;

  private:
    std::string _answer;
    std::binary_semaphore _released { 0 };
};

/// A loop and a worker, which the native probe is awaited on and runs its command on.
struct ProbeLoop
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend };
    core::async::ThreadPoolExecutor worker { 1 };

    /// @return What @p probe answers to @p query, awaited on the loop.
    [[nodiscard]] std::expected<ReadinessReport, UpgradeRefusal> ask(ReadinessProbe& probe,
                                                                     ReadinessQuery query)
    {
        return loop.blockOn(probe.probe(std::move(query)));
    }
};

/// @return A report naming exactly the formats this build writes.
[[nodiscard]] ReadinessReport currentFormats(std::string release = "0.9.0")
{
    return ReadinessReport {
        .release = std::move(release),
        .handoverFormats = { HandoverFormatVersion, HandoverFormatVersion },
        .terminalStateFormats = { vtbackend::TerminalStateFormatVersion,
                                  vtbackend::TerminalStateFormatVersion },
    };
}

} // namespace

TEST_CASE("a readiness report is one line of strict JSON", "[vthost][handover][upgrade]")
{
    auto const report = ReadinessReport { .release = "0.9.0-\"dev\"",
                                          .handoverFormats = { 1, 3 },
                                          .terminalStateFormats = { 2, 4 } };
    CHECK(encodeReadiness(report)
          == R"({"release":"0.9.0-\"dev\"","handoverFormats":[1,3],"terminalStateFormats":[2,4]})");
    CHECK(decodeReadiness(encodeReadiness(report)) == report);
}

TEST_CASE("this build reports the formats it reads", "[vthost][handover][upgrade]")
{
    auto const own = ownReadiness("1.2.3");
    CHECK(own.release == "1.2.3");
    CHECK(own.handoverFormats == std::pair { OldestReadableHandoverFormat, HandoverFormatVersion });
    CHECK(
        own.terminalStateFormats
        == std::pair { vtbackend::OldestReadableTerminalStateFormat, vtbackend::TerminalStateFormatVersion });
    CHECK(checkReadable(own).has_value());
}

TEST_CASE("a later release's readiness report is read past keys it adds", "[vthost][handover][upgrade]")
{
    auto const decoded = decodeReadiness(
        R"({"release":"2.0","handoverFormats":[1,2],"terminalStateFormats":[1,1],"compression":"zstd"})");
    REQUIRE(decoded.has_value());
    CHECK(decoded->release == "2.0");
    CHECK(decoded->handoverFormats == std::pair { 1U, 2U });
}

TEST_CASE("anything but a readiness report is refused", "[vthost][handover][upgrade]")
{
    CHECK_FALSE(decodeReadiness("").has_value());
    CHECK_FALSE(decodeReadiness("Unknown command: handover-info").has_value());
    CHECK_FALSE(decodeReadiness(R"({"release":"0.9.0"})").has_value());
    CHECK_FALSE(decodeReadiness(R"({"release":"0.9.0","handoverFormats":[1],"terminalStateFormats":[1,1]})")
                    .has_value());
    CHECK_FALSE(decodeReadiness(R"({"release":"0.9.0","handoverFormats":[2,1],"terminalStateFormats":[1,1]})")
                    .has_value());
}

TEST_CASE("a target that cannot read either format this build writes is unreadable",
          "[vthost][handover][upgrade]")
{
    CHECK(checkReadable(currentFormats()).has_value());
    auto newer = currentFormats();
    newer.handoverFormats = { HandoverFormatVersion + 1, HandoverFormatVersion + 2 };
    CHECK(checkReadable(newer) == std::unexpected(UpgradeRefusal::FormatUnreadable));
    auto older = currentFormats();
    older.terminalStateFormats = { 0, vtbackend::TerminalStateFormatVersion - 1 };
    CHECK(checkReadable(older) == std::unexpected(UpgradeRefusal::FormatUnreadable));
}

TEST_CASE("a target that reads only format 1 cannot take this build's image", "[vthost][handover][upgrade]")
{
    auto formatOne = currentFormats();
    formatOne.handoverFormats = { 1, 1 };
    CHECK(checkReadable(formatOne) == std::unexpected(UpgradeRefusal::FormatUnreadable));
}

TEST_CASE("a target that cannot run as the daemon's configuration says is refused",
          "[vthost][handover][upgrade]")
{
    auto report = currentFormats();
    CHECK(checkConfigured(report).has_value());
    report.configProblem = "cannot load config: bad indentation";
    CHECK(checkConfigured(report) == std::unexpected(UpgradeRefusal::ConfigUnreadable));
    // It travels in the report, and only when there is one.
    CHECK(decodeReadiness(encodeReadiness(report)) == report);
    CHECK_FALSE(encodeReadiness(currentFormats()).contains("configProblem"));
}

TEST_CASE("the readiness probe asks the target's query mode and reads its answer",
          "[vthost][handover][upgrade]")
{
    auto capture = FakeCapture { encodeReadiness(currentFormats("0.9.1")) };
    auto loop = ProbeLoop {};
    auto probe = CommandReadinessProbe { capture, loop.worker, 1234ms };
    auto const query = ReadinessQuery { .executable = "/state/bin/0.9.1-abcdef01/contour",
                                        .configFile = "/home/u/.config/contour/contour.yml",
                                        .profile = "work" };
    auto const answer = loop.ask(probe, query);
    REQUIRE(answer.has_value());
    CHECK(answer->release == "0.9.1");
    REQUIRE(capture.commands.size() == 1);
    CHECK(capture.commands.front() == readinessCommand(query));
    CHECK(capture.timeouts.front() == 1234ms);
}

TEST_CASE("the readiness probe names the configuration only when there is one", "[vthost][handover][upgrade]")
{
    auto const bare = readinessCommand(ReadinessQuery { .executable = "/x/contour" });
    CHECK(bare.contains("daemon handover-info"));
    CHECK_FALSE(bare.contains("--config"));
    auto const named = readinessCommand(
        ReadinessQuery { .executable = "/x/contour", .configFile = "/c/contour.yml", .profile = "p" });
    CHECK(named.contains("--config"));
    CHECK(named.contains("--profile"));
}

#ifdef _WIN32
// The probe runs the target itself, with no cmd.exe between: its command line is the target's own, each
// argument as the target's C runtime reads it back.
TEST_CASE("the readiness probe's command line is the target's own", "[vthost][handover][upgrade]")
{
    auto const command =
        readinessCommand(ReadinessQuery { .executable = "C:/Program Files/contour/contour.exe",
                                          .configFile = "C:/my files/contour.yml",
                                          .profile = "work" });
    CHECK(
        command
        == R"("C:/Program Files/contour/contour.exe" daemon handover-info --config "C:/my files/contour.yml")"
           R"( --profile work)");
}
#endif

TEST_CASE("the readiness probe waits off the loop, which serves on, and completes back on it",
          "[vthost][handover][upgrade][threading]")
{
    auto capture = GatedCapture { encodeReadiness(currentFormats("0.9.1")) };
    auto loop = ProbeLoop {};
    auto probe = CommandReadinessProbe { capture, loop.worker };
    auto ticks = std::atomic<int> { 0 };
    auto completedOn = std::thread::id {};
    auto const flow = [](core::net::EventLoop* on,
                         ReadinessProbe* asked,
                         GatedCapture* gate,
                         std::atomic<int>* count,
                         std::thread::id* resumedOn) -> core::async::Task<bool> {
        auto ticker =
            [](core::net::EventLoop* l, std::atomic<int>* n, GatedCapture* g) -> core::async::Task<void> {
            // The loop turns while the target has not answered; then the answer is let through.
            while (n->load() < 5)
            {
                co_await l->delay(std::chrono::milliseconds { 5 });
                ++*n;
            }
            g->release();
        };
        on->spawn(ticker(on, count, gate));
        auto const answer = co_await asked->probe(ReadinessQuery { .executable = "/x/contour" });
        *resumedOn = std::this_thread::get_id();
        co_return answer.has_value();
    };
    CHECK(loop.loop.blockOn(flow(&loop.loop, &probe, &capture, &ticks, &completedOn)));
    CHECK(ticks.load() >= 5);
    CHECK(capture.ranOn != std::this_thread::get_id());
    CHECK(completedOn == std::this_thread::get_id());
}

TEST_CASE("a target that does not answer is unresponsive, one that answers otherwise unreadable",
          "[vthost][handover][upgrade]")
{
    using Kind = vtpty::CommandCaptureError::Kind;
    auto loop = ProbeLoop {};
    for (auto const kind: { Kind::SpawnFailed, Kind::TimedOut, Kind::Cancelled })
    {
        auto capture =
            FakeCapture { std::unexpected(vtpty::CommandCaptureError { .kind = kind, .cause = {} }) };
        auto probe = CommandReadinessProbe { capture, loop.worker };
        CHECK(loop.ask(probe, { .executable = "/x/contour" })
              == std::unexpected(UpgradeRefusal::TargetUnresponsive));
    }
    auto capture = FakeCapture { std::string { "contour: unknown option" } };
    auto probe = CommandReadinessProbe { capture, loop.worker };
    CHECK(loop.ask(probe, { .executable = "/x/contour" })
          == std::unexpected(UpgradeRefusal::FormatUnreadable));
}

#ifndef _WIN32
TEST_CASE("the native probe runs the target by its exact path, in the daemon's own environment",
          "[vthost][handover][upgrade]")
{
    // What the exec'd target inherits, an AppImage's or Nix's library path among it: the probe sees it too.
    auto const library = core::testing::ScopedEnv { "LD_LIBRARY_PATH", "/nix/store/x-contour/lib" };
    auto const home = core::testing::ScopedEnv { "HOME", "/home/probe" };
    // A path a shell would split or unquote: the probe must still run exactly this file.
    auto const dir = core::testing::ScopedTempDir { "contour-readiness" };
    auto const executable = dir / "it's a \"target\" $HOME";
    {
        auto script = std::ofstream { executable };
        // Answers only to its query mode, and reports the environment it saw, and its configuration
        // arguments, as its release.
        script
            << "#!/bin/sh\n"
               "[ \"$1 $2\" = 'daemon handover-info' ] || exit 3\n"
               "printf '{\"release\":\"%s|%s|%s|%s|%s|%s|%s\",\"handoverFormats\":[1,1],"
               "\"terminalStateFormats\":[1,1]}\\n' \"$HOME\" \"$LD_LIBRARY_PATH\" \"$PATH\" \"$3\" \"$4\" "
               "\"$5\" \"$6\"\n";
    }
    std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
    auto capture = vtpty::SystemCommandCapture { "/bin/sh", core::async::StopToken {} };
    auto loop = ProbeLoop {};
    auto probe = CommandReadinessProbe { capture, loop.worker };
    auto const answer =
        loop.ask(probe, { .executable = executable, .configFile = "/c/my contour.yml", .profile = "it's" });
    REQUIRE(answer.has_value());
    auto const path = core::LiveEnvironment {}.get("PATH").value_or("");
    CHECK(answer->release
          == std::format("/home/probe|/nix/store/x-contour/lib|{}|--config|/c/my contour.yml|--profile|it's",
                         path));
}

TEST_CASE("the native probe gives up on a target that hangs", "[vthost][handover][upgrade]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-readiness" };
    auto const executable = dir / "contour";
    {
        auto script = std::ofstream { executable };
        script << "#!/bin/sh\nexec sleep 30\n";
    }
    std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
    auto capture = vtpty::SystemCommandCapture { "/bin/sh", core::async::StopToken {} };
    auto loop = ProbeLoop {};
    auto probe = CommandReadinessProbe { capture, loop.worker, 200ms };
    CHECK(loop.ask(probe, { .executable = executable })
          == std::unexpected(UpgradeRefusal::TargetUnresponsive));
}
#endif
