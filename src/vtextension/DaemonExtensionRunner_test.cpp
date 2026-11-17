// SPDX-License-Identifier: Apache-2.0
//
// The daemon world's runner, driven step by step: a scripted guest on the fake backend, a manual scheduler,
// and in-memory doubles of the processes, the store and the presence it serves the guest from.
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <latch>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/DaemonExtensionRunner.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonLimits.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionStore.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/testing/DaemonRunnerHarness.hpp>

using namespace std::chrono_literals;
using namespace vtextension;
using vtextension::testing::DaemonRunnerHarness;
using vtextension::testing::PublishedSummary;

namespace
{

/// Where a scripted guest has the host store a call's result.
constexpr auto ResultAddress = uint32_t { 256 };
/// Where a scripted guest puts its first string argument.
constexpr auto FirstText = uint32_t { 4096 };
/// Where a scripted guest puts its second string argument.
constexpr auto SecondText = uint32_t { 8192 };

using UnitResult = std::expected<void, HostError>;

/// What a scripted callback answers: it returned.
[[nodiscard]] std::expected<void, wasm::CallFailure> returned()
{
    return {};
}

/// What a scripted callback answers when an import it called failed: the call fails too.
[[nodiscard]] std::expected<void, wasm::CallFailure> passOn(std::expected<void, ExtensionError> const& call)
{
    if (!call)
        return std::unexpected(wasm::CallFailure::HostFailure);
    return {};
}

/// Has the guest call `store-set` of @p value under @p key.
/// @return What the host answered the guest; nothing if the call trapped.
[[nodiscard]] std::optional<UnitResult> storeSet(DaemonRunnerHarness& harness,
                                                 std::string_view key,
                                                 std::string_view value,
                                                 uint32_t keyAddress = FirstText,
                                                 uint32_t valueAddress = SecondText)
{
    harness.put(keyAddress, key);
    harness.put(valueAddress, value);
    auto const called = harness.call(DaemonImport::StoreSet,
                                     { keyAddress,
                                       static_cast<uint32_t>(key.size()),
                                       valueAddress,
                                       static_cast<uint32_t>(value.size()),
                                       ResultAddress });
    if (!called)
        return std::nullopt;
    auto answer = harness.read<UnitResult>(ResultAddress);
    REQUIRE(answer.has_value());
    return *answer;
}

/// Has the guest call `summary-set`.
/// @return What the host answered the guest.
[[nodiscard]] UnitResult summarySet(DaemonRunnerHarness& harness,
                                    std::string_view shortText,
                                    std::string_view detail)
{
    harness.put(FirstText, shortText);
    harness.put(SecondText, detail);
    REQUIRE(harness
                .call(DaemonImport::SummarySet,
                      { FirstText,
                        static_cast<uint32_t>(shortText.size()),
                        SecondText,
                        static_cast<uint32_t>(detail.size()),
                        ResultAddress })
                .has_value());
    auto answer = harness.read<UnitResult>(ResultAddress);
    REQUIRE(answer.has_value());
    return *answer;
}

} // namespace

namespace
{
/// A time zone 90 minutes ahead of UTC.
[[nodiscard]] std::chrono::local_seconds ninetyMinutesAhead(
    std::chrono::system_clock::time_point when) noexcept
{
    return std::chrono::local_seconds { std::chrono::floor<std::chrono::seconds>(when).time_since_epoch()
                                        + 90min };
}
} // namespace

TEST_CASE("DaemonExtensionRunner: load tells the roots, the home and the UTC offset of that load",
          "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    harness.localTime = &ninetyMinutesAhead;
    auto config = DaemonRunnerHarness::defaultConfig();
    config.roots = { ResolvedRoot { .label = "work", .paths = { "/w" } } };
    harness.start(std::move(config));
    REQUIRE(harness.loadedHostInfos.size() == 1);
    CHECK(harness.loadedHostInfos[0]
          == DaemonHostInfo { .roots = { ResolvedRoot { .label = "work", .paths = { "/w" } } },
                              .home = "/home/me",
                              .utcOffsetMinutes = 90 });
}

namespace
{
/// The instant a time zone's clocks go forward, from 60 to 120 minutes ahead of UTC.
constexpr auto OffsetChange = std::chrono::sys_seconds { std::chrono::hours { 24 } };

/// A time zone 60 minutes ahead of UTC before @ref OffsetChange and 120 minutes ahead from it on.
[[nodiscard]] std::chrono::local_seconds aheadAcrossAChange(
    std::chrono::system_clock::time_point when) noexcept
{
    auto const utc = std::chrono::floor<std::chrono::seconds>(when);
    return std::chrono::local_seconds { utc.time_since_epoch() + (utc < OffsetChange ? 60min : 120min) };
}
} // namespace

TEST_CASE("DaemonExtensionRunner: a later load reads the UTC offset again", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    harness.localTime = &aheadAcrossAChange;
    harness.wallClock.setNow(OffsetChange - 1h);
    harness.onUpdate = [] {
        return std::expected<void, wasm::CallFailure> { std::unexpect, wasm::CallFailure::Trap };
    };
    harness.start();
    REQUIRE(harness.loadedHostInfos.size() == 1);
    CHECK(harness.loadedHostInfos[0].utcOffsetMinutes == 60);

    // The clocks went forward while the guest ran; it faults and is loaded again.
    harness.wallClock.setNow(OffsetChange + 1h);
    harness.runner->hint(DaemonHint::Procs);
    harness.scheduler.runPending();
    harness.scheduler.advance(DaemonRunnerHarness::defaultConfig().limits.initialBackoff);
    REQUIRE(harness.loadedHostInfos.size() == 2);
    CHECK(harness.loadedHostInfos[1].utcOffsetMinutes == 120);
}

TEST_CASE("DaemonExtensionRunner: hints coalesce, one slot per kind", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    harness.start();
    REQUIRE(harness.runner->status().state == DaemonRunnerState::Running);

    harness.runner->hint(DaemonHint::Procs);
    harness.runner->hint(DaemonHint::Procs);
    harness.runner->hint(DaemonHint::Procs);
    harness.runner->hint(DaemonHint::Wake);
    harness.scheduler.runPending();

    // Delivered in the hint enum's order, not in the order they were raised.
    CHECK(harness.updates == std::vector { DaemonHint::Wake, DaemonHint::Procs });
}

TEST_CASE("DaemonExtensionRunner: wake honours the floor and replaces the previous timer", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    SECTION("the floor")
    {
        harness.onLoad = [&] {
            return passOn(harness.call(DaemonImport::Wake, { 5 }));
        };
        harness.start();
        CHECK(harness.scheduler.pendingTimers() == 1);
        harness.scheduler.advance(MinWakeInterval - 1ms);
        CHECK(harness.updates.empty());
        harness.scheduler.advance(1ms);
        CHECK(harness.updates == std::vector { DaemonHint::Wake });
    }
    SECTION("the later request replaces the earlier")
    {
        harness.onLoad = [&] {
            if (auto const early = harness.call(DaemonImport::Wake, { 5 }); !early)
                return passOn(early);
            return passOn(harness.call(DaemonImport::Wake, { 100 }));
        };
        harness.start();
        CHECK(harness.scheduler.pendingTimers() == 1);
        harness.scheduler.advance(99ms);
        CHECK(harness.updates.empty());
        harness.scheduler.advance(1ms);
        CHECK(harness.updates == std::vector { DaemonHint::Wake });
        harness.scheduler.advance(1s);
        CHECK(harness.updates.size() == 1);
    }
}

TEST_CASE("DaemonExtensionRunner: now reads the scheduler's clock and the wall clock", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    harness.scheduler.manualClock().setNow(core::platform::SteadyTimePoint { 12'345ms });
    harness.wallClock.setNow(std::chrono::system_clock::time_point { 1'763'000'000'123ms });
    harness.onLoad = [&] {
        return passOn(harness.call(DaemonImport::Now, { ResultAddress }));
    };
    harness.start();

    auto const reading = harness.read<ClockReading>(ResultAddress);
    REQUIRE(reading.has_value());
    CHECK(*reading == ClockReading { .monotonicMs = 12'345, .wallMs = 1'763'000'000'123 });
}

TEST_CASE("DaemonExtensionRunner: the per-call byte budget fits one full store value", "[daemon][runner]")
{
    constexpr auto MemoryBytes = std::size_t { 256 } * 1024;
    constexpr auto KeyAddress = uint32_t { 128 * 1024 };
    constexpr auto ValueAddress = KeyAddress + 1024;
    auto const key = std::string(StoreLimits {}.maxKeyBytes, 'k');
    auto harness = DaemonRunnerHarness {};
    auto answer = std::optional<UnitResult> {};
    auto valueBytes = std::size_t { 0 };
    harness.onUpdate = [&] {
        harness.guest().linearMemory.resize(MemoryBytes);
        answer = storeSet(harness, key, std::string(valueBytes, 'v'), KeyAddress, ValueAddress);
        return answer ? returned() : std::unexpected(wasm::CallFailure::HostFailure);
    };
    harness.start();

    SECTION("a 64 KiB value under a 128-byte key")
    {
        valueBytes = StoreLimits {}.maxValueBytes;
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        REQUIRE(answer.has_value());
        CHECK(answer->has_value());
        CHECK(harness.store.values.at(key).size() == StoreLimits {}.maxValueBytes);
        CHECK(harness.runner->status().state == DaemonRunnerState::Running);
    }
    SECTION("one byte more traps")
    {
        valueBytes = StoreLimits {}.maxValueBytes + 1;
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        CHECK_FALSE(answer.has_value());
        CHECK(harness.store.writes == 0);
        auto const status = harness.runner->status();
        CHECK(status.state == DaemonRunnerState::BackingOff);
        CHECK(status.lastFault == ExtensionError::Trapped);
    }
}

TEST_CASE("DaemonExtensionRunner: durable calls past the per-callback budget answer limit",
          "[daemon][runner]")
{
    auto const budget = DurableBudget {};
    auto harness = DaemonRunnerHarness {};
    auto answers = std::vector<std::optional<UnitResult>> {};
    auto calls = std::size_t { 0 };
    harness.onUpdate = [&] {
        for ([[maybe_unused]] auto const call: std::views::iota(std::size_t { 0 }, calls))
            answers.push_back(storeSet(harness, "key", "value"));
        return returned();
    };
    harness.start();

    SECTION("by count")
    {
        calls = budget.maxCalls + 1;
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        REQUIRE(answers.size() == budget.maxCalls + 1);
        for (auto const& answer: answers | std::views::take(budget.maxCalls))
            CHECK(answer == UnitResult {});
        CHECK(answers.back() == UnitResult { std::unexpect, HostError::Limit });
        CHECK(harness.store.writes == budget.maxCalls);
    }
    SECTION("by time")
    {
        calls = 2;
        harness.store.onWrite = [&] {
            harness.scheduler.manualClock().advance(budget.maxTime + 1ms);
        };
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        REQUIRE(answers.size() == 2);
        CHECK(answers.front() == UnitResult {});
        CHECK(answers.back() == UnitResult { std::unexpect, HostError::Limit });
        CHECK(harness.store.writes == 1);
        harness.store.onWrite = nullptr;
    }

    // The next callback has a fresh budget.
    answers.clear();
    calls = 1;
    auto const writesBefore = harness.store.writes;
    harness.runner->hint(DaemonHint::Wake);
    harness.scheduler.runPending();
    CHECK(answers == std::vector<std::optional<UnitResult>> { UnitResult {} });
    CHECK(harness.store.writes == writesBefore + 1);
    CHECK(harness.runner->status().state == DaemonRunnerState::Running);
}

TEST_CASE("DaemonExtensionRunner: a slow store write does not count toward the deadline", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    auto const deadline = DaemonRunnerHarness::defaultConfig().limits.callDeadline;
    // The disk takes longer than the whole call may: by the deadline's clock, with no real wait.
    harness.store.onWrite = [&] {
        harness.scheduler.manualClock().advance(deadline + 1ms);
    };
    auto answers = std::vector<std::optional<UnitResult>> {};
    harness.onUpdate = [&] {
        answers.push_back(storeSet(harness, "first", "1"));
        answers.push_back(storeSet(harness, "second", "2"));
        return returned();
    };
    harness.start();
    harness.runner->hint(DaemonHint::Procs);
    harness.scheduler.runPending();

    CHECK(answers == std::vector<std::optional<UnitResult>> { UnitResult {}, UnitResult {} });
    auto const status = harness.runner->status();
    CHECK(status.state == DaemonRunnerState::Running);
    CHECK_FALSE(status.lastFault.has_value());
}

TEST_CASE("DaemonExtensionRunner: a fault restarts the guest, which finds its processes by tag",
          "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    auto listed = std::optional<std::expected<std::vector<ProcInfo>, HostError>> {};
    harness.onLoad = [&] {
        if (harness.loadedConfigs.size() == 1)
        {
            harness.put(FirstText, "gh-auth");
            harness.put(SecondText, "poll/auth");
            return passOn(harness.call(DaemonImport::ProcStart,
                                       { FirstText, 7, 0, 0, 0, 0, SecondText, 9, ResultAddress }));
        }
        if (auto const called = harness.call(DaemonImport::Procs, { ResultAddress }); !called)
            return passOn(called);
        auto read = harness.read<std::expected<std::vector<ProcInfo>, HostError>>(ResultAddress);
        REQUIRE(read.has_value());
        listed = std::move(*read);
        return returned();
    };
    harness.onUpdate = [] {
        return std::expected<void, wasm::CallFailure> { std::unexpect, wasm::CallFailure::Trap };
    };
    harness.start();
    REQUIRE(harness.processes.processes.size() == 1);

    harness.runner->hint(DaemonHint::Procs);
    harness.scheduler.runPending();
    auto const faulted = harness.runner->status();
    CHECK(faulted.state == DaemonRunnerState::BackingOff);
    CHECK(faulted.lastFault == ExtensionError::Trapped);

    auto const backoff = DaemonRunnerHarness::defaultConfig().limits.initialBackoff;
    harness.scheduler.advance(backoff - 1ms);
    CHECK(harness.loadedConfigs.size() == 1);
    harness.scheduler.advance(1ms);
    REQUIRE(harness.loadedConfigs.size() == 2);
    CHECK(harness.loadedConfigs.back() == harness.loadedConfigs.front());
    CHECK(harness.runner->status().state == DaemonRunnerState::Running);
    REQUIRE(listed.has_value());
    REQUIRE(listed->has_value());
    REQUIRE((*listed)->size() == 1);
    CHECK((*listed)->front().tag == "poll/auth");
    CHECK((*listed)->front().command == "gh-auth");
    CHECK((*listed)->front().state == ProcState { ProcRunning {} });
}

TEST_CASE("DaemonExtensionRunner: three faults in 60 s disable the extension", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    harness.onUpdate = [] {
        return std::expected<void, wasm::CallFailure> { std::unexpect, wasm::CallFailure::Trap };
    };
    harness.start();
    for ([[maybe_unused]] auto const fault: std::views::iota(0, 3))
    {
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        harness.scheduler.advance(5s); // Past any back-off, well within the window.
    }

    auto const status = harness.runner->status();
    CHECK(status.state == DaemonRunnerState::Disabled);
    CHECK(status.lastFault == ExtensionError::Trapped);
    CHECK(status.faultsInWindow == 3);
    CHECK(harness.logged == std::vector<std::string> { "orchestrator disabled: extension trapped" });
    CHECK(harness.loadedConfigs.size() == 3);
    harness.scheduler.advance(10min);
    CHECK(harness.loadedConfigs.size() == 3);

    harness.runner->restart();
    harness.scheduler.runPending();
    CHECK(harness.loadedConfigs.size() == 4);
    CHECK(harness.runner->status().state == DaemonRunnerState::Running);
    CHECK(harness.runner->status().faultsInWindow == 0);
    // Its presence was told each change, as the run it is: up, disabled, and up again.
    using Told = std::pair<DaemonRunGeneration, DaemonExtensionPhase>;
    CHECK(harness.presence.phases()
          == std::vector { Told { 0, DaemonExtensionPhase::Running },
                           Told { 0, DaemonExtensionPhase::Disabled },
                           Told { 0, DaemonExtensionPhase::Running } });

    harness.runner->restart();
    harness.scheduler.runPending();
    CHECK(harness.loadedConfigs.size() == 4);
}

TEST_CASE("DaemonExtensionRunner: destruction with a queued hint is safe", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    harness.start();
    harness.runner->hint(DaemonHint::Procs);
    harness.runner.reset();
    harness.scheduler.runPending();

    CHECK(harness.updates.empty());
    CHECK(harness.retirement.count() == 1);
}

TEST_CASE("DaemonExtensionRunner: a not-supported-yet import does not fault", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    auto answer = std::optional<std::expected<std::string, HostError>> {};
    harness.onUpdate = [&] {
        auto arguments = std::vector<uint32_t>(
            DaemonImportRows.at(std::to_underlying(DaemonImport::PaneOpen)).parameterCount, 0);
        arguments.back() = ResultAddress;
        if (auto const called = harness.call(DaemonImport::PaneOpen, arguments); !called)
            return passOn(called);
        auto read = harness.read<std::expected<std::string, HostError>>(ResultAddress);
        REQUIRE(read.has_value());
        answer = std::move(*read);
        return returned();
    };
    harness.start();
    harness.runner->hint(DaemonHint::Wake);
    harness.scheduler.runPending();

    CHECK(answer == std::expected<std::string, HostError> { std::unexpect, HostError::NotSupportedYet });
    CHECK(harness.runner->status().state == DaemonRunnerState::Running);
}

TEST_CASE("DaemonExtensionRunner: summary-set is bounded and neutralized", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    auto shortText = std::string {};
    auto detail = std::string {};
    auto answer = std::optional<UnitResult> {};
    harness.onUpdate = [&] {
        answer = summarySet(harness, shortText, detail);
        return returned();
    };
    harness.start();
    auto const set = [&](std::string shortSummary, std::string longer) {
        shortText = std::move(shortSummary);
        detail = std::move(longer);
        harness.runner->hint(DaemonHint::Wake);
        harness.scheduler.runPending();
        REQUIRE(answer.has_value());
        return *answer;
    };

    SECTION("neutralized, the detail line by line")
    {
        CHECK(set("\x1b[31mx", "one\x1b\ntwo\u200b") == UnitResult {});
        CHECK(harness.presence.published()
              == std::vector { PublishedSummary { .extension = "orchestrator",
                                                  .shortText = "<U+001B>[31mx",
                                                  .detail = "one<U+001B>\ntwo<U+200B>" } });
    }
    SECTION("the one-line summary is counted in characters")
    {
        CHECK(set(std::string(MaxSummaryShortChars + 1, 'a'), "")
              == UnitResult { std::unexpect, HostError::Limit });
        CHECK(harness.presence.published().empty());
        auto accented = std::string {};
        for ([[maybe_unused]] auto const character: std::views::iota(std::size_t { 0 }, MaxSummaryShortChars))
            accented += "\u00e9";
        CHECK(set(accented, "") == UnitResult {});
        CHECK(harness.presence.published().size() == 1);
    }
    SECTION("the detail is counted in bytes")
    {
        CHECK(set("s", std::string(MaxSummaryDetailBytes, 'd')) == UnitResult {});
        REQUIRE(harness.presence.published().size() == 1);
        CHECK(harness.presence.published().front().detail.size() == MaxSummaryDetailBytes);
        CHECK(set("s", std::string(MaxSummaryDetailBytes + 1, 'd'))
              == UnitResult { std::unexpect, HostError::Limit });
        CHECK(harness.presence.published().size() == 1);
    }
    SECTION("the cap is on the input: its worst case, neutralized, grows eightfold and no more")
    {
        CHECK(set("s", std::string(MaxSummaryDetailBytes, '\x01')) == UnitResult {});
        REQUIRE(harness.presence.published().size() == 1);
        CHECK(harness.presence.published().front().detail.size() == MaxSummaryDetailShownBytes);
    }
    CHECK(harness.runner->status().state == DaemonRunnerState::Running);
}

TEST_CASE("DaemonExtensionRunner: store errors reach the guest as host errors", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    auto answer = std::optional<UnitResult> {};
    harness.onUpdate = [&] {
        answer = storeSet(harness, "key", "value");
        return returned();
    };
    harness.start();

    for (auto const& row: StoreErrorToHostError)
    {
        CAPTURE(std::to_underlying(row.error));
        harness.store.failWith = row.error;
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        REQUIRE(answer.has_value());
        CHECK(*answer == UnitResult { std::unexpect, row.hostError });
    }
    CHECK(hostErrorOf(StoreError::Io) == HostError::Gone);
    CHECK(hostErrorOf(StoreError::TooLarge) == HostError::Limit);
    CHECK(harness.runner->status().state == DaemonRunnerState::Running);
}

TEST_CASE("DaemonExtensionRunner: an update past the deadline is a fault", "[daemon][runner][threading]")
{
    auto harness = DaemonRunnerHarness {};
    auto config = DaemonRunnerHarness::defaultConfig();
    config.limits.callDeadline = 20ms;
    // The guest spins until the engine's watchdog interrupts it; bounded, so that a watchdog that never fires
    // fails the checks rather than hanging the test.
    harness.onUpdate = [&] {
        auto const giveUp = std::chrono::steady_clock::now() + 5s;
        while (harness.guest().interrupts.load() == 0 && std::chrono::steady_clock::now() < giveUp)
            std::this_thread::yield();
        return std::expected<void, wasm::CallFailure> { std::unexpect, wasm::CallFailure::Interrupted };
    };
    harness.start(config);
    harness.runner->hint(DaemonHint::Procs);
    harness.scheduler.runPending();

    auto const status = harness.runner->status();
    CHECK(status.lastFault == ExtensionError::DeadlineExceeded);
    CHECK(status.state == DaemonRunnerState::BackingOff);
    CHECK(harness.scheduler.pendingTimers() == 1);
}

TEST_CASE("DaemonExtensionRunner: a hint raised during a drain is delivered on its own", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    harness.onUpdate = [&] {
        if (harness.updates.back() == DaemonHint::Procs)
            harness.runner->hint(DaemonHint::Tabs);
        return returned();
    };
    harness.start();
    harness.runner->hint(DaemonHint::Procs);
    harness.scheduler.runPending();

    // No other hint is raised: the one raised while the lane drained schedules a drain of its own.
    CHECK(harness.updates == std::vector { DaemonHint::Procs, DaemonHint::Tabs });
}

TEST_CASE("DaemonExtensionRunner: hints wait for the next instance", "[daemon][runner]")
{
    auto harness = DaemonRunnerHarness {};
    // The first instance faults on its first update; every later one returns.
    harness.onUpdate = [&] {
        return harness.loadedConfigs.size() == 1
                   ? std::expected<void, wasm::CallFailure> { std::unexpect, wasm::CallFailure::Trap }
                   : returned();
    };

    SECTION("the hints after a faulted one, and those raised while backing off")
    {
        harness.start();
        harness.runner->hint(DaemonHint::Tabs);
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        REQUIRE(harness.runner->status().state == DaemonRunnerState::BackingOff);
        CHECK(harness.updates == std::vector { DaemonHint::Procs });

        harness.runner->hint(DaemonHint::Wake);
        harness.scheduler.runPending();
        CHECK(harness.updates.size() == 1);

        harness.scheduler.advance(DaemonRunnerHarness::defaultConfig().limits.initialBackoff);
        REQUIRE(harness.loadedConfigs.size() == 2);
        // The faulted `procs` is dropped; `tabs`, after it, waited; `wake` was raised without an instance.
        CHECK(harness.updates == std::vector { DaemonHint::Procs, DaemonHint::Wake, DaemonHint::Tabs });
    }
    SECTION("the hints raised while disabled")
    {
        auto config = DaemonRunnerHarness::defaultConfig();
        config.limits.faultsBeforeDisable = 1;
        harness.start(config);
        harness.runner->hint(DaemonHint::Procs);
        harness.scheduler.runPending();
        REQUIRE(harness.runner->status().state == DaemonRunnerState::Disabled);

        harness.runner->hint(DaemonHint::Actions);
        harness.scheduler.advance(10min);
        CHECK(harness.updates == std::vector { DaemonHint::Procs });

        harness.runner->restart();
        harness.scheduler.runPending();
        REQUIRE(harness.loadedConfigs.size() == 2);
        CHECK(harness.updates == std::vector { DaemonHint::Procs, DaemonHint::Actions });
    }
}

TEST_CASE("DaemonExtensionRunner: hints raised from another thread while the lane drains are not lost",
          "[daemon][runner][threading]")
{
    constexpr auto Rounds = 2000;
    auto harness = DaemonRunnerHarness {};
    auto actionsSeen = std::atomic<int> { 0 };
    auto actionsDelivered = std::latch { 1 };
    harness.onUpdate = [&] {
        // Counted down once only: a second delivery is the failure the checks below report.
        if (harness.updates.back() == DaemonHint::Actions && actionsSeen.fetch_add(1) == 0)
            actionsDelivered.count_down();
        return returned();
    };
    auto pool = ExtensionWorkerPool(harness.backend, 1);
    harness.startOn(pool);

    for (auto const round: std::views::iota(0, Rounds))
        harness.runner->hint(round % 2 == 0 ? DaemonHint::Procs : DaemonHint::Wake);
    // The last hint, raised once: it must reach the guest however the drains interleaved.
    harness.runner->hint(DaemonHint::Actions);
    actionsDelivered.wait();

    harness.runner.reset();
    pool.stop();
    CHECK(actionsSeen.load() == 1);
    CHECK(std::ranges::count(harness.updates, DaemonHint::Actions) == 1);
    CHECK(std::ranges::all_of(harness.updates, [](DaemonHint hint) {
        return hint == DaemonHint::Procs || hint == DaemonHint::Wake || hint == DaemonHint::Actions;
    }));
    CHECK(harness.retirement.count() == 1);
}
