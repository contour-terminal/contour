// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/StatusLineCommandCache.hpp>

#include <vtpty/CommandCapture.hpp>

#include <core/async/testing/ManualExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <expected>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using vtbackend::StatusLineCommandCache;
using vtbackend::StatusLineCommandOutput;
using Completions = std::vector<StatusLineCommandOutput>;
using vtpty::CommandCaptureError;

namespace
{
/// Answers runs from a script, and records what it was asked.
struct FakeCapture final: vtpty::CommandCapture
{
    std::deque<std::expected<std::string, CommandCaptureError>> answers;
    std::vector<std::string> commands;
    std::vector<std::chrono::milliseconds> timeouts;
    int stoppedRuns = 0; ///< Runs that started with a stop already requested.

    std::expected<std::string, CommandCaptureError> run(std::string_view command,
                                                        std::chrono::milliseconds timeout,
                                                        core::async::StopToken stop) override
    {
        commands.emplace_back(command);
        timeouts.push_back(timeout);
        if (stop.stop_requested())
            ++stoppedRuns;
        if (answers.empty())
            return std::unexpected(CommandCaptureError { CommandCaptureError::Kind::SpawnFailed,
                                                         std::make_error_code(std::errc::invalid_argument) });
        auto answer = std::move(answers.front());
        answers.pop_front();
        return answer;
    }
};

struct Fixture
{
    FakeCapture capture;
    core::async::testing::ManualExecutor executor;
    std::vector<StatusLineCommandOutput> completions;
    StatusLineCommandCache cache { { capture, executor },
                                   [this](StatusLineCommandOutput o) { completions.push_back(o); } };
    StatusLineCommandCache::Clock::time_point t0 {};
};
} // namespace

TEST_CASE("StatusLineCommandCache.emptyUntilFirstResultThenNotifies", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("main");
    CHECK(f.cache.output("git branch", 10s, f.t0).empty());
    CHECK(f.capture.commands.empty()); // nothing ran on the caller's thread
    f.executor.drain();
    REQUIRE(f.capture.commands == std::vector<std::string> { "git branch" });
    CHECK(f.capture.timeouts.front() == 5000ms);
    CHECK(f.completions == Completions { StatusLineCommandOutput::Changed });
    CHECK(f.cache.output("git branch", 10s, f.t0 + 1s) == "main");
}

TEST_CASE("StatusLineCommandCache.noOverlapWhileInFlight", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("x");
    (void) f.cache.output("cmd", 1s, f.t0);
    (void) f.cache.output("cmd", 1s, f.t0 + 5s); // due, but the first run has not finished
    f.executor.drain();
    CHECK(f.capture.commands.size() == 1);
}

TEST_CASE("StatusLineCommandCache.rerunsOnlyAfterInterval", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("1");
    f.capture.answers.emplace_back("2");
    (void) f.cache.output("cmd", 10s, f.t0);
    f.executor.drain();
    (void) f.cache.output("cmd", 10s, f.t0 + 9s);
    f.executor.drain();
    CHECK(f.capture.commands.size() == 1);
    (void) f.cache.output("cmd", 10s, f.t0 + 10s);
    f.executor.drain();
    CHECK(f.capture.commands.size() == 2);
    CHECK(f.cache.output("cmd", 10s, f.t0 + 11s) == "2");
}

TEST_CASE("StatusLineCommandCache.timeoutBoundedByInterval", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("x");
    (void) f.cache.output("cmd", 2s, f.t0);
    f.executor.drain();
    CHECK(f.capture.timeouts.front() == 2000ms);
}

TEST_CASE("StatusLineCommandCache.unchangedOutputNotifiesAsUnchanged", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("same");
    f.capture.answers.emplace_back("same");
    (void) f.cache.output("cmd", 1s, f.t0);
    f.executor.drain();
    (void) f.cache.output("cmd", 1s, f.t0 + 1s);
    f.executor.drain();
    CHECK(f.completions
          == Completions { StatusLineCommandOutput::Changed, StatusLineCommandOutput::Unchanged });
}

TEST_CASE("StatusLineCommandCache.timeoutKeepsLastGoodOutput", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("good");
    f.capture.answers.emplace_back(
        std::unexpected(CommandCaptureError { CommandCaptureError::Kind::TimedOut, {} }));
    (void) f.cache.output("cmd", 1s, f.t0);
    f.executor.drain();
    (void) f.cache.output("cmd", 1s, f.t0 + 1s);
    f.executor.drain();
    CHECK(f.cache.output("cmd", 1s, f.t0 + 1500ms) == "good");
    CHECK(f.completions
          == Completions { StatusLineCommandOutput::Changed, StatusLineCommandOutput::Unchanged });
}

TEST_CASE("StatusLineCommandCache.spawnFailureShowsMessage", "[statusline][commandcache]")
{
    auto f = Fixture {};
    auto const cause = std::make_error_code(std::errc::no_such_file_or_directory);
    f.capture.answers.emplace_back(
        std::unexpected(CommandCaptureError { CommandCaptureError::Kind::SpawnFailed, cause }));
    (void) f.cache.output("cmd", 1s, f.t0);
    f.executor.drain();
    CHECK(f.cache.output("cmd", 1s, f.t0 + 100ms) == cause.message());
}

TEST_CASE("StatusLineCommandCache.evictsUnrequestedEntries", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("a");
    f.capture.answers.emplace_back("b");
    (void) f.cache.output("old", 1s, f.t0);
    f.executor.drain();
    (void) f.cache.output("new", 1s, f.t0 + 3s); // "old" unrequested for > 2 × 1 s → evicted
    f.executor.drain();
    REQUIRE(f.cache.nextDue(f.t0 + 3s).has_value());
    CHECK(*f.cache.nextDue(f.t0 + 3s) == 1000ms); // only "new" remains, due 1 s after it started
}

TEST_CASE("StatusLineCommandCache.nextDue", "[statusline][commandcache]")
{
    auto f = Fixture {};
    CHECK_FALSE(f.cache.nextDue(f.t0).has_value());
    f.capture.answers.emplace_back("x");
    (void) f.cache.output("cmd", 10s, f.t0);
    CHECK(f.cache.nextDue(f.t0 + 4s) == 6000ms); // in flight, not yet overdue: counts toward the wakeup
    CHECK_FALSE(f.cache.nextDue(f.t0 + 11s).has_value()); // in flight and overdue: do not spin
    f.executor.drain();
    CHECK(f.cache.nextDue(f.t0 + 4s) == 6000ms);
    CHECK(f.cache.nextDue(f.t0 + 12s) == 0ms);
}

TEST_CASE("StatusLineCommandCache.destroyedWithRunInFlight", "[statusline][commandcache]")
{
    auto capture = FakeCapture {};
    capture.answers.emplace_back("late");
    auto executor = core::async::testing::ManualExecutor {};
    auto notifications = 0;
    {
        auto cache = StatusLineCommandCache { { capture, executor },
                                              [&](StatusLineCommandOutput) { ++notifications; } };
        (void) cache.output("cmd", 1s, {});
    } // destroyed before the job runs
    executor.drain(); // the job runs against the surviving state and must not notify
    CHECK(capture.commands.size() == 1);
    CHECK(capture.stoppedRuns == 1); // the destructor requested stop
    CHECK(notifications == 0);
}

TEST_CASE("StatusLineCommandCache.neverEvictsRunningEntry", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("a");
    f.capture.answers.emplace_back("b");
    (void) f.cache.output("a", 1s, f.t0);
    (void) f.cache.output("b", 1s, f.t0 + 3s); // "a" is stale but still running: kept
    (void) f.cache.output("a", 1s, f.t0 + 3s); // so it is not started a second time
    f.executor.drain();
    REQUIRE(f.capture.commands.size() == 2);
}

TEST_CASE("StatusLineCommandCache.cancelledKeepsTextWithoutNotifying", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("good");
    f.capture.answers.emplace_back(
        std::unexpected(CommandCaptureError { CommandCaptureError::Kind::Cancelled, {} }));
    (void) f.cache.output("cmd", 1s, f.t0);
    f.executor.drain();
    (void) f.cache.output("cmd", 1s, f.t0 + 1s);
    f.executor.drain();
    CHECK(f.cache.output("cmd", 1s, f.t0 + 1500ms) == "good");
    CHECK(f.completions == Completions { StatusLineCommandOutput::Changed });
}

TEST_CASE("StatusLineCommandCache.forgetRequestsStopsWakeups", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("x");
    (void) f.cache.output("cmd", 1s, f.t0);
    f.executor.drain();
    REQUIRE(f.cache.nextDue(f.t0 + 5s) == 0ms); // overdue idle entry
    f.cache.forgetRequests();
    CHECK_FALSE(f.cache.nextDue(f.t0 + 5s).has_value());
}

TEST_CASE("StatusLineCommandCache.rerequestAfterForgetKeepsTextAndDoesNotRerun", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("x");
    (void) f.cache.output("cmd", 10s, f.t0);
    f.executor.drain();
    f.cache.forgetRequests();
    CHECK(f.cache.output("cmd", 10s, f.t0 + 2s) == "x");
    f.executor.drain();
    CHECK(f.capture.commands.size() == 1);
    CHECK(f.cache.nextDue(f.t0 + 2s) == 8000ms);
}

TEST_CASE("StatusLineCommandCache.forgottenEntryCompletesSilently", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("x");
    (void) f.cache.output("cmd", 1s, f.t0);
    f.cache.forgetRequests(); // while the run is in flight
    f.executor.drain();
    CHECK(f.completions.empty());
    CHECK(f.cache.output("cmd", 1s, f.t0 + 100ms) == "x");
}

TEST_CASE("StatusLineCommandCache.forgetKeepsAllCarriedOverEntries", "[statusline][commandcache]")
{
    auto f = Fixture {};
    f.capture.answers.emplace_back("a");
    f.capture.answers.emplace_back("b");
    (void) f.cache.output("A", 10s, f.t0);
    (void) f.cache.output("B", 10s, f.t0);
    f.executor.drain();
    f.cache.forgetRequests();
    CHECK(f.cache.output("A", 10s, f.t0 + 2s) == "a");
    CHECK(f.cache.output("B", 10s, f.t0 + 2s) == "b");
    f.executor.drain();
    CHECK(f.capture.commands.size() == 2);
}
