// SPDX-License-Identifier: Apache-2.0
//
// The engine on its own: the watchdog that stops a runaway call. The runtime's lifetime and thread
// preparation are the backend's, tested with it (wasm/testing/BackendContract.hpp). The watchdog is
// driven against a fake instance, which counts the interrupts it receives, and once end to end against
// a spinning guest on the default backend.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ThreadCpuClock.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

using namespace std::string_view_literals;
using vtextension::DeadlineState;
using vtextension::Engine;
using vtextension::ExtensionError;
using vtextension::ExtensionLimits;
using vtextension::IThreadCpuClock;
using vtextension::ThreadCpuId;
using vtextension::WatchToken;
using vtextension::testing::FakeWasmBackend;
using vtextension::testing::FakeWasmInstance;

namespace
{

/// How long a test waits for the watchdog before it gives up.
constexpr auto Eventually = std::chrono::seconds { 2 };

/// @return @p duration in fractional milliseconds, for reporting.
[[nodiscard]] double toMilliseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

/// @return An engine on @p backend, which must be created.
[[nodiscard]] std::shared_ptr<Engine> createEngine(vtextension::wasm::IWasmBackend& backend,
                                                   ExtensionLimits limits = {})
{
    auto engine = Engine::create(backend, vtextension::World::StatusLine, limits);
    REQUIRE(engine.has_value());
    return std::move(*engine);
}

/// A thread CPU clock the test sets: every thread reads what the test stored, or none can be named.
class FakeThreadCpuClock final: public IThreadCpuClock
{
  public:
    [[nodiscard]] std::optional<ThreadCpuId> currentThread() const noexcept override
    {
        if (!isReadable)
            return std::nullopt;
        return ThreadCpuId { .value = 1 };
    }

    [[nodiscard]] std::optional<std::chrono::nanoseconds> consumed(
        ThreadCpuId /*thread*/) const noexcept override
    {
        return cpu.load();
    }

    std::atomic<std::chrono::nanoseconds> cpu { std::chrono::nanoseconds::zero() };
    bool isReadable = true;
};

/// @return Limits whose deadlines may last @p factor times as long in real time.
[[nodiscard]] ExtensionLimits realTimeFactorOf(std::uint32_t factor)
{
    auto limits = ExtensionLimits {};
    limits.deadlineRealTimeFactor = factor;
    return limits;
}

/// @return An engine on @p backend whose deadlines @p cpuClock measures, which must be created.
[[nodiscard]] std::shared_ptr<Engine> createEngine(vtextension::wasm::IWasmBackend& backend,
                                                   ExtensionLimits limits,
                                                   IThreadCpuClock const& cpuClock)
{
    auto engine = Engine::create(backend, vtextension::World::Daemon, limits, cpuClock);
    REQUIRE(engine.has_value());
    return std::move(*engine);
}

/// Waits until @p instance was interrupted, or Eventually passed.
/// @return Whether it was interrupted in time.
[[nodiscard]] bool awaitInterrupt(FakeWasmInstance const& instance)
{
    auto const giveUp = std::chrono::steady_clock::now() + Eventually;
    while (instance.interrupts.load() == 0 && std::chrono::steady_clock::now() < giveUp)
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    return instance.interrupts.load() != 0;
}

/// What the end-to-end guest's host context answers: nothing calls it, the guest imports nothing.
class NoHost final: public vtextension::wasm::IHostContext
{
  public:
    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        return std::unexpected(ExtensionError::Trapped);
    }
};

/// A module needing no toolchain: (module (func (export "spin") (loop (br 0))) (func (export "idle"))).
/// It keeps the engine's tests independent of any toolchain.
[[nodiscard]] std::vector<std::byte> hostOnlyModule()
{
    auto const bytes = std::vector<std::uint8_t> {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,                   // magic, version
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                               // type 0: () -> ()
        0x03, 0x03, 0x02, 0x00, 0x00,                                     // funcs 0, 1: type 0
        0x07, 0x0f, 0x02, 0x04, 's',  'p',  'i',  'n',  0x00, 0x00,       // export "spin" = 0
        0x04, 'i',  'd',  'l',  'e',  0x00, 0x01,                         // export "idle" = 1
        0x0a, 0x0c, 0x02, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b, // 0: loop { br 0 }
        0x02, 0x00, 0x0b,                                                 // 1: (empty)
    };
    auto module = std::vector<std::byte>(bytes.size());
    std::ranges::transform(bytes, module.begin(), [](std::uint8_t byte) { return std::byte { byte }; });
    return module;
}

} // namespace

TEST_CASE("Engine.create")
{
    auto backend = FakeWasmBackend {};
    auto const engine = createEngine(backend);
    CHECK(engine->limits().callDeadline == ExtensionLimits {}.callDeadline);
    CHECK(&engine->backend() == &backend);
}

TEST_CASE("Engine.fakeBackendBringUps")
{
    // The fake keeps the bring-up rules every backend must: no guard while no runtime is up, and a new
    // generation each time an engine brings a runtime up anew.
    auto backend = FakeWasmBackend {};
    auto const refused = backend.enterThread();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::RuntimeUnavailable);
    CHECK_FALSE(backend.isCurrentThreadPrepared());
    CHECK(backend.runtimeGeneration() == 0);

    auto engine = createEngine(backend);
    auto const first = backend.enterThread();
    REQUIRE(first.has_value());
    CHECK((*first)->generation() == 1);
    CHECK(backend.isCurrentThreadPrepared());
    auto joined = createEngine(backend); // joins the bring-up rather than starting one
    CHECK(backend.runtimeGeneration() == 1);

    engine.reset();
    CHECK(backend.isCurrentThreadPrepared()); // a runtime is still up
    joined.reset();
    CHECK_FALSE(backend.isCurrentThreadPrepared());
    CHECK_FALSE(backend.enterThread().has_value());

    auto const again = createEngine(backend);
    CHECK(backend.runtimeGeneration() == 2);
}

TEST_CASE("Engine.deadline")
{
    // On expiry the watchdog marks the deadline expired, then interrupts the instance, once.
    auto backend = FakeWasmBackend {};
    auto const engine = createEngine(backend);
    auto instance = FakeWasmInstance {};
    auto state = std::atomic<DeadlineState> { DeadlineState::Running };

    engine->armDeadline(WatchToken { .instance = &instance, .state = &state },
                        std::chrono::milliseconds { 20 });

    REQUIRE(awaitInterrupt(instance));
    CHECK(instance.interrupts.load() == 1);
    CHECK(state.load() == DeadlineState::Expired);
}

TEST_CASE("Engine.disarm")
{
    // A disarmed deadline never fires: the instance is not interrupted, the deadline not expired.
    auto backend = FakeWasmBackend {};
    auto const engine = createEngine(backend);
    auto instance = FakeWasmInstance {};
    auto state = std::atomic<DeadlineState> { DeadlineState::Running };
    auto const token = WatchToken { .instance = &instance, .state = &state };

    engine->armDeadline(token, std::chrono::milliseconds { 50 });
    engine->disarm(token);
    std::this_thread::sleep_for(std::chrono::milliseconds { 200 });

    CHECK(instance.interrupts.load() == 0);
    CHECK(state.load() == DeadlineState::Running);
}

TEST_CASE("Engine.disarmBeforeDestroy")
{
    // A deadline about to fire as its instance is torn down: once disarm() returns, the watchdog does not
    // touch the instance or its state, which are destroyed right after (ASan checks that it does not). A
    // deadline fires at most once, and nothing arrives after disarm(): the last round keeps its instance
    // alive well past another deadline period to see that nothing does.
    constexpr auto Rounds = 200;
    constexpr auto Deadline = std::chrono::milliseconds { 1 };
    auto backend = FakeWasmBackend {};
    auto const engine = createEngine(backend);
    for (auto const round: std::views::iota(0, Rounds))
    {
        auto instance = FakeWasmInstance {};
        auto state = std::atomic<DeadlineState> { DeadlineState::Running };
        auto const token = WatchToken { .instance = &instance, .state = &state };
        engine->armDeadline(token, Deadline);
        auto const until = std::chrono::steady_clock::now() + Deadline;
        while (std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        engine->disarm(token);

        auto const interrupts = instance.interrupts.load();
        CHECK(interrupts <= 1);
        if (round == Rounds - 1)
        {
            std::this_thread::sleep_for(20 * Deadline);
            CHECK(instance.interrupts.load() == interrupts);
        }
    }
}

TEST_CASE("Engine.armWhileWatching")
{
    // The watchdog sleeps towards the earliest deadline while other threads arm more: arming may move the
    // deadlines in memory, and the watchdog must not be left holding on to where the earliest one was
    // (ASan checks that it is not).
    auto backend = FakeWasmBackend {};
    auto const engine = createEngine(backend);
    auto instances = std::vector<FakeWasmInstance>(256);
    auto states = std::vector<std::atomic<DeadlineState>>(instances.size());
    auto const tokenOf = [&](std::size_t index) {
        return WatchToken { .instance = &instances[index], .state = &states[index] };
    };

    for (auto const index: std::views::iota(std::size_t { 0 }, instances.size()))
        engine->armDeadline(tokenOf(index), std::chrono::hours { 1 });
    for (auto const index: std::views::iota(std::size_t { 0 }, instances.size()))
        engine->disarm(tokenOf(index));

    CHECK(std::ranges::all_of(
        instances, [](FakeWasmInstance const& instance) { return instance.interrupts.load() == 0; }));
}

TEST_CASE("Engine.deadline.endToEnd")
{
    // End to end on the default backend: the deadline stops a guest spinning in an endless loop, and a call
    // that finished in time leaves its instance usable once its deadline is disarmed.
    auto const backend = vtextension::wasm::makeDefaultBackend();
    auto limits = ExtensionLimits {};
    limits.callDeadline = std::chrono::milliseconds { 50 };
    auto const engine = createEngine(*backend, limits);
    auto module = engine->runtime().load(hostOnlyModule());
    REQUIRE(module.has_value());

    auto const callUnderDeadline = [&](vtextension::wasm::IWasmInstance& instance, std::string_view name) {
        auto state = std::atomic<DeadlineState> { DeadlineState::Running };
        auto const token = WatchToken { .instance = &instance, .state = &state };
        auto const function = instance.function(name);
        REQUIRE(function.has_value());
        engine->armDeadline(token, limits.callDeadline);
        auto const called = instance.call(*function, {});
        engine->disarm(token);
        return std::pair { called, state.load() };
    };

    SECTION("spin")
    {
        auto host = NoHost {};
        auto const instance = (*module)->instantiate(host);
        REQUIRE(instance.has_value());
        auto const start = std::chrono::steady_clock::now();
        auto const [called, state] = callUnderDeadline(**instance, "spin");
        auto const elapsed = std::chrono::steady_clock::now() - start;

        // The bound only proves that termination happened at all; how soon is the scheduler's business.
        INFO("spin stopped after " << toMilliseconds(elapsed) << " ms (deadline "
                                   << limits.callDeadline.count() << " ms)");
        REQUIRE_FALSE(called.has_value());
        CHECK(called.error() == vtextension::wasm::CallFailure::Interrupted);
        CHECK(state == DeadlineState::Expired);
        CHECK(elapsed < 10 * limits.loadDeadline);
    }

    SECTION("idle")
    {
        auto host = NoHost {};
        auto const instance = (*module)->instantiate(host);
        REQUIRE(instance.has_value());
        CHECK(callUnderDeadline(**instance, "idle").first.has_value());
        std::this_thread::sleep_for(4 * limits.callDeadline);
        auto const [again, state] = callUnderDeadline(**instance, "idle");
        CHECK(again.has_value());
        CHECK(state == DeadlineState::Running);
    }
}

TEST_CASE("Engine.deadline.waitingForCpuIsNotCharged")
{
    // A thread that spent none of its CPU budget is not interrupted when the budget has passed in real
    // time; it is once it spends the budget.
    auto backend = FakeWasmBackend {};
    auto cpuClock = FakeThreadCpuClock {};
    auto const engine = createEngine(backend, realTimeFactorOf(100), cpuClock);
    auto instance = FakeWasmInstance {};
    auto state = std::atomic<DeadlineState> { DeadlineState::Running };
    constexpr auto Budget = std::chrono::milliseconds { 20 };

    engine->armDeadline(WatchToken { .instance = &instance, .state = &state }, Budget);
    std::this_thread::sleep_for(5 * Budget);
    CHECK(instance.interrupts.load() == 0);
    CHECK(state.load() == DeadlineState::Running);

    cpuClock.cpu = Budget;
    REQUIRE(awaitInterrupt(instance));
    CHECK(instance.interrupts.load() == 1);
    CHECK(state.load() == DeadlineState::Expired);
}

TEST_CASE("Engine.deadline.realTimeBound")
{
    // A thread that never gets a CPU is interrupted all the same, once its deadline times the real-time
    // factor has passed, and not before.
    auto backend = FakeWasmBackend {};
    auto cpuClock = FakeThreadCpuClock {};
    constexpr auto Factor = 4U;
    auto const engine = createEngine(backend, realTimeFactorOf(Factor), cpuClock);
    auto instance = FakeWasmInstance {};
    auto state = std::atomic<DeadlineState> { DeadlineState::Running };
    constexpr auto Budget = std::chrono::milliseconds { 20 };

    auto const start = std::chrono::steady_clock::now();
    engine->armDeadline(WatchToken { .instance = &instance, .state = &state }, Budget);
    REQUIRE(awaitInterrupt(instance));
    auto const elapsed = std::chrono::steady_clock::now() - start;

    INFO("interrupted after " << toMilliseconds(elapsed) << " ms");
    CHECK(elapsed >= Factor * Budget);
    CHECK(state.load() == DeadlineState::Expired);
}

TEST_CASE("Engine.deadline.unreadableClockIsRealTime")
{
    // Where the thread's clock cannot be read, the deadline is spent in real time, factor or not.
    auto backend = FakeWasmBackend {};
    auto cpuClock = FakeThreadCpuClock {};
    cpuClock.isReadable = false;
    auto const engine = createEngine(backend, realTimeFactorOf(100), cpuClock);
    auto instance = FakeWasmInstance {};
    auto state = std::atomic<DeadlineState> { DeadlineState::Running };

    auto const start = std::chrono::steady_clock::now();
    engine->armDeadline(WatchToken { .instance = &instance, .state = &state },
                        std::chrono::milliseconds { 20 });
    REQUIRE(awaitInterrupt(instance));
    CHECK(std::chrono::steady_clock::now() - start < Eventually);
}

TEST_CASE("Engine.unspent")
{
    // What is left is the CPU budget less what the thread spent; nothing once disarmed.
    auto backend = FakeWasmBackend {};
    auto cpuClock = FakeThreadCpuClock {};
    auto const engine = createEngine(backend, realTimeFactorOf(100), cpuClock);
    auto instance = FakeWasmInstance {};
    auto state = std::atomic<DeadlineState> { DeadlineState::Running };
    auto const token = WatchToken { .instance = &instance, .state = &state };

    CHECK(engine->unspent(token) == std::chrono::nanoseconds::zero());
    engine->armDeadline(token, std::chrono::milliseconds { 100 });
    cpuClock.cpu = std::chrono::milliseconds { 30 };
    CHECK(engine->unspent(token) == std::chrono::milliseconds { 70 });
    engine->disarm(token);
    CHECK(engine->unspent(token) == std::chrono::nanoseconds::zero());
}

TEST_CASE("Engine.deadline.endToEndInCpuTime")
{
    // On the native thread clock, a guest spinning in an endless loop spends its budget and is stopped, real
    // time ten times as long as its budget notwithstanding.
    auto const backend = vtextension::wasm::makeDefaultBackend();
    auto const engine = createEngine(*backend, realTimeFactorOf(10), vtextension::nativeThreadCpuClock());
    auto module = engine->runtime().load(hostOnlyModule());
    REQUIRE(module.has_value());
    auto host = NoHost {};
    auto const instance = (*module)->instantiate(host);
    REQUIRE(instance.has_value());
    auto const function = (*instance)->function("spin");
    REQUIRE(function.has_value());

    constexpr auto Budget = std::chrono::milliseconds { 50 };
    auto state = std::atomic<DeadlineState> { DeadlineState::Running };
    auto const token = WatchToken { .instance = instance->get(), .state = &state };
    engine->armDeadline(token, Budget);
    auto const called = (*instance)->call(*function, {});
    engine->disarm(token);

    REQUIRE_FALSE(called.has_value());
    CHECK(called.error() == vtextension::wasm::CallFailure::Interrupted);
    CHECK(state.load() == DeadlineState::Expired);
}

TEST_CASE("Engine.nativeThreadCpuClock")
{
    // The calling thread's clock can be named and read, and it advances while the thread computes.
    auto const& clock = vtextension::nativeThreadCpuClock();
    auto const thread = clock.currentThread();
    REQUIRE(thread.has_value());
    auto const before = clock.consumed(*thread);
    REQUIRE(before.has_value());
    auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds { 50 };
    auto spins = std::uint64_t { 0 };
    while (std::chrono::steady_clock::now() < until)
        ++spins;
    auto const after = clock.consumed(*thread);
    REQUIRE(after.has_value());
    CHECK(spins > 0);
    CHECK(*after > *before);
}

TEST_CASE("Engine.errorText")
{
    CHECK(vtextension::errorText(ExtensionError::DeadlineExceeded)
          == "extension call exceeded its deadline"sv);
    CHECK(vtextension::errorText(ExtensionError::OutputTooLarge) == "extension output too large"sv);
}
