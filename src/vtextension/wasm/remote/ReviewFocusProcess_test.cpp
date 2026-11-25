// SPDX-License-Identifier: Apache-2.0
//
// What a real extension host process must never do to Contour: keep a lane when it dies in the middle of a
// host call, make an interrupt wait on it, run on past an interrupt, or have each pane start a helper of its
// own after a crash. Each case composes its own supervisor, so that its counts are its own; each ends by
// checking that no helper ended otherwise than the case made it end (D57).
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionRegistry.hpp>
#include <vtextension/HostBreaker.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/Placement.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/RemoteKindState.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/types.h>

    #include <csignal>
#endif

namespace process = vtextension::wasm::remote::process;
namespace contract = vtextension::wasm::testing::contract;
using process::KillReason;
using process::testing::HelperBackendConfig;
using process::testing::HelperComposition;
using vtextension::ExtensionError;
using vtextension::wasm::CallFailure;
using vtextension::wasm::HostTimeouts;
using vtextension::wasm::IHostContext;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::IWasmInstance;
using vtextension::wasm::LockdownRequirement;
using namespace std::chrono_literals;

namespace
{

/// How long any one thing a case waits for may take; it fails, rather than hang, beyond it.
constexpr auto Patience = std::chrono::seconds { 5 };

/// @return The real helper, held to @p timeouts.
[[nodiscard]] HelperBackendConfig realHelper(HostTimeouts timeouts = {})
{
    return HelperBackendConfig { .helper = VTEXTENSION_HELPER_PATH,
                                 .extraArguments = {},
                                 .lockdown = LockdownRequirement::AcceptUnsandboxed,
                                 .timeouts = timeouts };
}

/// Ends the test run, rather than wait forever on a thread or a future that a regression keeps from
/// finishing: their destructors would join or wait.
[[noreturn]] void abandon(char const* what)
{
    FAIL_CHECK(what);
    std::println(stderr, "ReviewFocusProcess: {}", what);
    std::abort();
}

/// @return @p future's value, once ready within Patience; else the run ends (abandon()).
template <typename T>
[[nodiscard]] T within(std::future<T>& future, char const* what)
{
    if (future.wait_for(Patience) != std::future_status::ready)
        abandon(what);
    return future.get();
}

/// Kills process @p processId as something outside Contour would: not through the supervisor.
void killFromOutside(process::ProcessId processId)
{
#ifdef _WIN32
    auto* const handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(processId));
    REQUIRE(handle != nullptr);
    CHECK(::TerminateProcess(handle, 9));
    ::CloseHandle(handle);
#else
    REQUIRE(::kill(static_cast<pid_t>(processId), SIGKILL) == 0);
#endif
}

/// Waits until @p composition's supervisor observed @p deaths helper ends, within Patience.
/// @return Whether it did.
[[nodiscard]] bool awaitDeaths(HelperComposition const& composition, std::size_t deaths)
{
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    while (std::chrono::steady_clock::now() < deadline)
    {
        composition.supervisor().sync();
        if (composition.supervisor().counters().deathsObserved >= deaths)
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

/// Waits until one of @p composition's lanes is held, then a little more, so that the call holding it is
/// running in the helper by then.
void awaitRunningCall(HelperComposition const& composition)
{
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    while (composition.kind()->lanesInUse() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(composition.kind()->lanesInUse() == 1);
    std::this_thread::sleep_for(100ms);
}

/// A host context whose `ping` blocks until released: a host call the parent is still serving.
class LatchedHostContext final: public IHostContext
{
  public:
    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        _entered.count_down();
        auto lock = std::unique_lock { _mutex };
        // Bounded, so that a case that never releases it does not hang the run.
        std::ignore = _released.wait_for(lock, 2 * Patience, [this] { return _isReleased; });
        return {};
    }

    /// @return Whether a host call entered within Patience.
    [[nodiscard]] bool awaitEntered() { return _entered.try_wait() || waitEntered(); }

    /// Lets every host call return.
    void release()
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _isReleased = true;
        }
        _released.notify_all();
    }

  private:
    [[nodiscard]] bool waitEntered()
    {
        auto const deadline = std::chrono::steady_clock::now() + Patience;
        while (!_entered.try_wait())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    std::latch _entered { 1 };
    std::mutex _mutex;
    std::condition_variable _released;
    bool _isReleased = false; ///< Guarded by _mutex.
};

/// What a call made on a thread of its own came to, and when it returned.
struct TimedCall
{
    std::expected<void, CallFailure> result;
    std::chrono::steady_clock::time_point returned;
};

/// Calls @p instance's export @p name with @p argv on a thread of its own, prepared by @p backend.
/// @return The call's outcome, once it returned.
[[nodiscard]] std::future<TimedCall> callAsync(IWasmBackend& backend,
                                               IWasmInstance& instance,
                                               std::string_view name,
                                               std::array<uint32_t, 2> argv = {})
{
    return std::async(std::launch::async, [&backend, &instance, name, argv]() mutable {
        auto const guard = backend.enterThread();
        if (!guard)
            return TimedCall { .result = std::unexpected(CallFailure::Trap),
                               .returned = std::chrono::steady_clock::now() };
        auto const function = instance.function(name);
        if (!function)
            return TimedCall { .result = std::unexpected(CallFailure::Trap),
                               .returned = std::chrono::steady_clock::now() };
        auto const result = instance.call(*function, argv);
        return TimedCall { .result = result, .returned = std::chrono::steady_clock::now() };
    });
}

/// @return Whether `add(2, 3)` on a fresh instance of @p module, made on a thread prepared by @p backend,
///         returns 5.
[[nodiscard]] bool addsOnAFreshInstance(IWasmBackend& backend, vtextension::wasm::IWasmModule& module)
{
    auto const sum = contract::onPreparedThread(backend, [&]() -> std::optional<uint32_t> {
        auto context = contract::RecordingHostContext {};
        auto instance = module.instantiate(context);
        if (!instance)
            return std::nullopt;
        auto argv = std::array<uint32_t, 2> { 2, 3 };
        if (!(*instance)->call(contract::functionOf(**instance, "add"), argv))
            return std::nullopt;
        return argv[0];
    });
    return sum && *sum == std::optional<uint32_t> { 5 };
}

} // namespace

TEST_CASE("helper death during CallHost frees the lane", "[vtextension][process]")
{
    auto const composition = HelperComposition {};
    {
        auto const backend = composition.backend(realHelper());
        auto const runtime = contract::createRuntime(*backend);
        auto const module = contract::load(*runtime, contract::InstanceModule);
        auto context = LatchedHostContext {};
        auto const instance = contract::instantiate(*module, context);

        auto call = callAsync(*backend, *instance, "ping-then-add", { 2, 3 });
        if (!context.awaitEntered())
        {
            context.release();
            abandon("the call never reached its host call");
        }
        // The helper dies while the parent serves its host call.
        killFromOutside(composition.recording().processIds().at(0));
        CHECK(awaitDeaths(composition, 1));
        context.release();

        auto const called = within(call, "the call did not return once its helper died");
        CHECK(called.result
              == std::expected<void, CallFailure> { std::unexpected(CallFailure::HostCrashed) });
        CHECK(composition.killsFor(KillReason::Grace) == 0);
        // The call's thread went, and its lane with it.
        CHECK(composition.kind()->lanesInUse() == 0);

        CHECK(addsOnAFreshInstance(*backend, *module));
        CHECK(composition.recording().launches() == 2);
    }
    CHECK(composition.awaitEveryHelperReaped());
    // The one the case killed, and no other.
    CHECK(composition.supervisor().counters().uncleanExits == 1);
}

#ifndef _WIN32
TEST_CASE("interrupt never blocks on a full pipe", "[vtextension][process]")
{
    // A helper that reads nothing: every interrupt raises the slot, fills the wake signal's buffer and wakes
    // the supervisor, and none of it waits. Windows' wake signal is an event, which never fills
    // ("WakeSignal.signalNeverBlocks").
    constexpr auto Interrupts = 100'000;
    auto const composition = HelperComposition {};
    {
        auto const backend = composition.backend(realHelper(HostTimeouts { .interruptGrace = 10s }));
        auto const runtime = contract::createRuntime(*backend);
        auto const module = contract::load(*runtime, contract::InstanceModule);
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(*module, context);

        auto spin = callAsync(*backend, *instance, "spin");
        awaitRunningCall(composition);
        auto const helper = static_cast<pid_t>(composition.recording().processIds().at(0));
        REQUIRE(::kill(helper, SIGSTOP) == 0);
        auto interrupting = std::async(std::launch::async, [&] {
            for ([[maybe_unused]] auto const index: std::views::iota(0, Interrupts))
                instance->interrupt();
            return true;
        });
        auto const isInterrupted = interrupting.wait_for(Patience) == std::future_status::ready;
        REQUIRE(::kill(helper, SIGCONT) == 0);
        CHECK(isInterrupted);
        std::ignore = within(interrupting, "interrupting a stopped helper blocked");

        // Running again, the helper takes the wake, finds the slot raised, and stops the call.
        auto const spun = within(spin, "the interrupted call did not return");
        CHECK(spun.result == std::expected<void, CallFailure> { std::unexpected(CallFailure::Interrupted) });
        CHECK(composition.killsFor(KillReason::Grace) == 0);
    }
    CHECK(composition.awaitEveryHelperReaped());
    CHECK(composition.supervisor().counters().uncleanExits == 0);
}
#endif

TEST_CASE("helper ignoring interrupt is killed after grace", "[vtextension][process]")
{
    constexpr auto Grace = std::chrono::milliseconds { 50 };
    auto const composition = HelperComposition {};
    {
        auto const backend =
            composition.backend(HelperBackendConfig { .helper = VTEXTENSION_HOSTILE_HELPER_PATH,
                                                      .extraArguments = { "--hostile=ignore-interrupt" },
                                                      .lockdown = LockdownRequirement::AcceptUnsandboxed,
                                                      .timeouts = HostTimeouts { .interruptGrace = Grace } });
        auto const runtime = contract::createRuntime(*backend);
        auto const module = contract::load(*runtime, contract::InstanceModule);
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(*module, context);

        auto spin = callAsync(*backend, *instance, "spin");
        awaitRunningCall(composition);
        auto const interruptedAt = std::chrono::steady_clock::now();
        instance->interrupt();

        auto const spun = within(spin, "the helper ignoring the interrupt was not stopped");
        CHECK(spun.result == std::expected<void, CallFailure> { std::unexpected(CallFailure::HostCrashed) });
        CHECK(spun.returned - interruptedAt >= Grace);
        CHECK(spun.returned - interruptedAt < Patience);
        CHECK(composition.killsFor(KillReason::Grace) == 1);

        // The next instance runs in a helper started anew.
        auto again = contract::RecordingHostContext {};
        CHECK(module->instantiate(again).has_value());
        CHECK(composition.recording().launches() == 2);
    }
    CHECK(composition.awaitEveryHelperReaped());
    CHECK(composition.supervisor().counters().uncleanExits == 0);
}

TEST_CASE("ten panes respawn one helper", "[vtextension][process]")
{
    using namespace vtextension;
    constexpr auto Panes = 10;
    constexpr auto Limits = ExtensionLimits {};
    auto const composition = HelperComposition {};
    {
        auto const backend = composition.backend(realHelper());
        auto const engine = [&] {
            auto created = Engine::create(*backend, Limits);
            REQUIRE(created.has_value());
            return std::move(*created);
        }();
        auto const threadGuard = vtextension::testing::preparedThread(*backend);
        auto clock = core::platform::ManualClock {};
        auto registry = ExtensionRegistry { clock, Limits };
        auto services = vtextension::testing::FakeHostServices {};
        auto const spec =
            ExtensionSpec { .name = "clock",
                            .hostId = "clock@/ext/minimal.wasm",
                            .module = vtextension::testing::toModule(vtextension::testing::MinimalModule),
                            .config = {},
                            .placement = Placement::Segment,
                            .runCommand = CommandPermission::Deny };
        auto const compiled = [&] {
            auto acquired =
                registry.acquire(*engine, registryKeyOf(spec, ExtensionWorld::StatusLine), spec.module);
            REQUIRE(acquired.has_value());
            return std::move(*acquired);
        }();
        auto const instantiate = [&] {
            return Instance::create(*engine, *compiled, services, clock, Limits);
        };

        auto panes = std::vector<std::unique_ptr<Instance>> {};
        for ([[maybe_unused]] auto const pane: std::views::iota(0, Panes))
        {
            auto instance = instantiate();
            REQUIRE(instance.has_value());
            panes.push_back(std::move(*instance));
        }
        REQUIRE(composition.recording().launches() == 1);

        killFromOutside(composition.recording().processIds().at(0));
        REQUIRE(awaitDeaths(composition, 1));
        for (auto const& pane: panes)
            CHECK(pane->render(vtextension::testing::surface(10, 1)).error()
                  == ExtensionError::HostUnavailable);
        CHECK(compiled->breaker()->faultCount() == 1);

        // Ten panes come back at once: one helper is started for all of them.
        auto start = std::latch { 1 };
        auto created =
            std::array<std::optional<std::expected<std::unique_ptr<Instance>, ExtensionError>>, Panes> {};
        {
            auto creators = std::vector<std::jthread> {};
            for (auto& slot: created)
                creators.emplace_back([&start, &slot, &instantiate] {
                    start.wait();
                    slot.emplace(instantiate());
                });
            start.count_down();
        }
        for (auto& result: created)
        {
            REQUIRE(result.has_value());
            REQUIRE(result->has_value());
        }
        CHECK(composition.recording().launches() == 2);
        composition.supervisor().sync();
        CHECK(composition.supervisor().liveProcesses() == 1);
        for (auto& result: created)
            CHECK((**result)->render(vtextension::testing::surface(10, 1)).has_value());
    }
    CHECK(composition.awaitEveryHelperReaped());
    // The one the case killed, and no other.
    CHECK(composition.supervisor().counters().uncleanExits == 1);
}
