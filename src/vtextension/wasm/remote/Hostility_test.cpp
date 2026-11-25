// SPDX-License-Identifier: Apache-2.0
//
// What a hostile extension host may do to Contour, and what Contour makes of it: a host that crashes at a
// step of the protocol, answers garbage or more than it may, stalls mid-answer, stops reading, or holds an
// answer back. Each is a row of contour-extension-host-hostile's table (HostileModes.hpp), started in place
// of the real host; each must cost Contour one failed operation reporting the right error within a bound,
// one host death, and one breaker fault -- and the next operation runs in a host started anew. Each case
// composes its own supervisor, so that its counts are its own.
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <contour-extension-host/testing/HostileModes.hpp>
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
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>

namespace process = vtextension::wasm::remote::process;
namespace contract = vtextension::wasm::testing::contract;
using contour::extension_host::testing::hostileModeNamed;
using contour::extension_host::testing::HostileOption;
using process::testing::HelperBackendConfig;
using process::testing::HelperComposition;
using process::testing::HelperTeardownGrace;
using vtextension::ExtensionError;
using vtextension::wasm::CallFailure;
using vtextension::wasm::HostTimeouts;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::IWasmInstance;
using vtextension::wasm::IWasmModule;
using vtextension::wasm::IWasmRuntime;
using vtextension::wasm::LockdownRequirement;
using vtextension::wasm::MemoryFailure;
using vtextension::wasm::ModuleIdentity;
using namespace std::chrono_literals;

namespace
{

/// The budgets the hostile hosts are held to: short, so that a hang costs little.
constexpr auto Timeouts =
    HostTimeouts { .load = 200ms, .instantiate = 200ms, .handshake = 200ms, .interruptGrace = 50ms };

/// How long after a call or a read began the watchdog interrupts it, as the pool's would at its deadline.
constexpr auto WatchdogDelay = 50ms;

/// How long an operation on a hostile host may take to fail.
constexpr auto FailureBound = 2s;

/// How long any one thing a case waits for may take; it fails, rather than hang, beyond it.
constexpr auto Patience = 5s;

/// How long a whole case may take; the run ends beyond it (CaseDeadline).
constexpr auto CaseBound = 60s;

/// Ends the test run once a case ran for longer than CaseBound: whatever a regression made hang -- a call,
/// a join, a future -- the run fails rather than hang.
class CaseDeadline
{
  public:
    explicit CaseDeadline(std::string_view name):
        _timer { [name](std::stop_token const& stop) {
            auto mutex = std::mutex {};
            auto woken = std::condition_variable_any {};
            auto lock = std::unique_lock { mutex };
            std::ignore = woken.wait_for(lock, stop, CaseBound, [] { return false; });
            if (stop.stop_requested())
                return;
            std::println(stderr, "Hostility: {} ran for longer than {}", name, CaseBound);
            std::abort();
        } }
    {
    }

  private:
    std::jthread _timer;
};

/// Interrupts an instance WatchdogDelay after it was made, unless destroyed before: a call or a read under
/// the pool's watchdog, whose deadline is up by then.
class Watchdog
{
  public:
    explicit Watchdog(IWasmInstance& instance):
        _thread { [&instance](std::stop_token const& stop) {
            auto mutex = std::mutex {};
            auto woken = std::condition_variable_any {};
            auto lock = std::unique_lock { mutex };
            std::ignore = woken.wait_for(lock, stop, WatchdogDelay, [] { return false; });
            if (!stop.stop_requested())
                instance.interrupt();
        } }
    {
    }

  private:
    std::jthread _thread;
};

/// @return @p arguments without the hostile mode: the helper started after a hostile one is benign.
[[nodiscard]] std::vector<std::string> withoutHostileMode(std::vector<std::string> arguments)
{
    std::erase_if(arguments, [](std::string const& argument) { return argument.starts_with(HostileOption); });
    return arguments;
}

/// @return The hostile helper in mode @p mode, held to Timeouts.
[[nodiscard]] HelperBackendConfig hostileHelper(std::string_view mode)
{
    REQUIRE(hostileModeNamed(mode) != nullptr);
    return HelperBackendConfig { .helper = VTEXTENSION_HOSTILE_HELPER_PATH,
                                 .extraArguments = { std::string { HostileOption } + std::string { mode } },
                                 .lockdown = LockdownRequirement::AcceptUnsandboxed,
                                 .timeouts = Timeouts };
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

/// What an operation on a hostile host is.
enum class Operation : uint8_t
{
    Add,         ///< `call("add", {2, 3})`, under the watchdog.
    PingThenAdd, ///< `call("ping-then-add", {2, 3})`, a host call first, under the watchdog.
    Read,        ///< `memory(16, 5)`, under the watchdog.
    Load,        ///< `runtime->load(...)`.
    Instantiate, ///< `module->instantiate(...)`.
};

/// How an operation failed, of the ways that matter here.
enum class Failure : uint8_t
{
    None,            ///< It did not.
    CallCrashed,     ///< CallFailure::HostCrashed.
    ReadCrashed,     ///< MemoryFailure::HostCrashed.
    HostUnavailable, ///< ExtensionError::HostUnavailable.
    Other,           ///< Any other way.
};

/// How the hostile helper ended.
enum class HelperEnd : uint8_t
{
    Killed,   ///< By the supervisor, as Contour stopped it: no unclean exit.
    OnItsOwn, ///< It crashed or exited: one unclean exit (D57).
};

/// One hostile mode, what Contour asks of it, and what that must come to.
struct HostileRow
{
    std::string_view mode;
    Operation operation;
    Failure failure;
    HelperEnd end;
};

constexpr auto HostileRows = std::array {
    HostileRow { "abort", Operation::Add, Failure::CallCrashed, HelperEnd::OnItsOwn },
    HostileRow { "segfault", Operation::Add, Failure::CallCrashed, HelperEnd::OnItsOwn },
    HostileRow { "garbage", Operation::Add, Failure::CallCrashed, HelperEnd::Killed },
    HostileRow { "stop-reading", Operation::Add, Failure::CallCrashed, HelperEnd::Killed },
    HostileRow { "exit-in-callhost", Operation::PingThenAdd, Failure::CallCrashed, HelperEnd::OnItsOwn },
    HostileRow { "over-cap", Operation::Read, Failure::ReadCrashed, HelperEnd::Killed },
    HostileRow { "bogus-size", Operation::Read, Failure::ReadCrashed, HelperEnd::Killed },
    HostileRow { "stall-after-header", Operation::Read, Failure::ReadCrashed, HelperEnd::Killed },
    HostileRow { "hang-at-handshake", Operation::Load, Failure::HostUnavailable, HelperEnd::Killed },
    HostileRow { "hang-at-load", Operation::Load, Failure::HostUnavailable, HelperEnd::Killed },
    HostileRow { "hang-at-instantiate", Operation::Instantiate, Failure::HostUnavailable, HelperEnd::Killed },
};

[[nodiscard]] Failure failureOf(CallFailure failure) noexcept
{
    return failure == CallFailure::HostCrashed ? Failure::CallCrashed : Failure::Other;
}

[[nodiscard]] Failure failureOf(MemoryFailure failure) noexcept
{
    return failure == MemoryFailure::HostCrashed ? Failure::ReadCrashed : Failure::Other;
}

[[nodiscard]] Failure failureOf(ExtensionError error) noexcept
{
    return error == ExtensionError::HostUnavailable ? Failure::HostUnavailable : Failure::Other;
}

/// @return How @p result failed.
template <typename T, typename E>
[[nodiscard]] Failure failureOf(std::expected<T, E> const& result) noexcept
{
    return result ? Failure::None : failureOf(result.error());
}

/// @return The instance module's bytes, loaded on @p runtime.
[[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> loadInstanceModule(
    IWasmRuntime& runtime)
{
    return runtime.load(ModuleIdentity {}, vtextension::testing::toModule(contract::InstanceModule));
}

/// @return How @p name, called with {2, 3} on @p instance under the watchdog, on a thread prepared by
///         @p backend, failed.
[[nodiscard]] Failure callUnderWatchdog(IWasmBackend& backend, IWasmInstance& instance, std::string_view name)
{
    return contract::onPreparedThread(backend,
                                      [&] {
                                          auto argv = std::array<uint32_t, 2> { 2, 3 };
                                          auto const function = contract::functionOf(instance, name);
                                          auto const watchdog = Watchdog { instance };
                                          return failureOf(instance.call(function, argv));
                                      })
        .value_or(Failure::Other);
}

/// @return Whether `add(2, 3)` on a fresh instance of @p module, made on a thread prepared by @p backend,
///         returns 5.
[[nodiscard]] bool addsOnAFreshInstance(IWasmBackend& backend, IWasmModule& module)
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

/// Runs @p row's operation on @p runtime, whose backend is @p backend.
/// @return How it failed, and the module it leaves to recover with: the one it ran on, or for a load, none.
[[nodiscard]] std::pair<Failure, std::shared_ptr<IWasmModule>> operate(HostileRow const& row,
                                                                       IWasmBackend& backend,
                                                                       IWasmRuntime& runtime)
{
    if (row.operation == Operation::Load)
        return { failureOf(loadInstanceModule(runtime)), nullptr };
    auto module = loadInstanceModule(runtime);
    REQUIRE(module.has_value());
    auto context = contract::RecordingHostContext {};
    if (row.operation == Operation::Instantiate)
        return { failureOf((*module)->instantiate(context)), *module };
    auto const instance = contract::instantiate(**module, context);
    switch (row.operation)
    {
        case Operation::Add: return { callUnderWatchdog(backend, *instance, "add"), *module };
        case Operation::PingThenAdd:
            return { callUnderWatchdog(backend, *instance, "ping-then-add"), *module };
        case Operation::Read:
            return { contract::onPreparedThread(backend,
                                                [&] {
                                                    auto const watchdog = Watchdog { *instance };
                                                    return failureOf(instance->memory(16, 5));
                                                })
                         .value_or(Failure::Other),
                     *module };
        case Operation::Load:
        case Operation::Instantiate: break;
    }
    return { Failure::Other, *module };
}

} // namespace

TEST_CASE("Hostility.modes", "[vtextension][process][hostility]")
{
    for (auto const& row: HostileRows)
    {
        DYNAMIC_SECTION(row.mode)
        {
            auto const deadline = CaseDeadline { row.mode };
            auto const composition = HelperComposition { HelperTeardownGrace, &withoutHostileMode };
            auto const expectedUnclean =
                row.end == HelperEnd::OnItsOwn ? std::size_t { 1 } : std::size_t { 0 };
            {
                auto const backend = composition.backend(hostileHelper(row.mode));
                auto const runtime = contract::createRuntime(*backend);

                auto const began = std::chrono::steady_clock::now();
                auto [failure, module] = operate(row, *backend, *runtime);
                auto const elapsed = std::chrono::steady_clock::now() - began;
                CHECK(failure == row.failure);
                CHECK(elapsed < FailureBound);

                // One death, whether the helper died on its own or was killed: a kill is reaped and observed
                // like any end.
                CHECK(awaitDeaths(composition, 1));
                composition.supervisor().sync();
                CHECK(composition.supervisor().counters().deathsObserved == 1);
                CHECK(composition.recording().launches() == 1);
                CHECK(composition.supervisor().counters().uncleanExits == expectedUnclean);

                // The next operation runs in a host started anew, which plays by the rules.
                if (!module)
                {
                    auto loaded = loadInstanceModule(*runtime);
                    REQUIRE(loaded.has_value());
                    module = *std::move(loaded);
                }
                CHECK(addsOnAFreshInstance(*backend, *module));
                CHECK(composition.recording().launches() == 2);
            }
            CHECK(composition.awaitEveryHelperReaped());
            // The one that misbehaved, if it died on its own; the one started anew exits cleanly.
            CHECK(composition.supervisor().counters().uncleanExits == expectedUnclean);
        }
    }
}

namespace
{

/// An extension of the statusline world over a composition of hostile helpers, as a pane builds one: an
/// engine, a registry with its breakers, and the MinimalModule extension's spec.
class HostileExtension
{
  public:
    /// @param mode The hostile mode of every helper started.
    /// @param laterArguments Applied to the command line of every helper after the first.
    HostileExtension(std::string_view mode, process::testing::ArgumentRewrite laterArguments):
        _composition { HelperTeardownGrace, std::move(laterArguments) },
        _backend { _composition.backend(hostileHelper(mode)) },
        _engine { [this] {
            auto created = vtextension::Engine::create(*_backend, Limits);
            REQUIRE(created.has_value());
            return std::move(*created);
        }() },
        _threadGuard { vtextension::testing::preparedThread(*_backend) },
        _registry { _clock, Limits }
    {
    }

    /// @return The extension, loaded or reused by the registry.
    [[nodiscard]] std::expected<std::shared_ptr<vtextension::CompiledExtension>, ExtensionError> acquire()
    {
        return _registry.acquire(
            *_engine, registryKeyOf(_spec, vtextension::ExtensionWorld::StatusLine), _spec.module);
    }

    /// @return A pane's instance of @p compiled.
    [[nodiscard]] std::expected<std::unique_ptr<vtextension::Instance>, ExtensionError> create(
        vtextension::CompiledExtension& compiled)
    {
        return vtextension::Instance::create(*_engine, compiled, _services, _clock, Limits);
    }

    /// @return What started the helpers.
    [[nodiscard]] HelperComposition const& composition() const noexcept { return _composition; }

  private:
    static constexpr auto Limits = vtextension::ExtensionLimits {};

    HelperComposition _composition;
    std::unique_ptr<IWasmBackend> _backend;
    std::shared_ptr<vtextension::Engine> _engine;
    std::unique_ptr<vtextension::wasm::IThreadGuard> _threadGuard;
    core::platform::ManualClock _clock;
    vtextension::ExtensionRegistry _registry;
    vtextension::testing::FakeHostServices _services;
    vtextension::ExtensionSpec _spec {
        .name = "clock",
        .hostId = "clock@/ext/minimal.wasm",
        .module = vtextension::testing::toModule(vtextension::testing::MinimalModule),
        .config = {},
        .placement = vtextension::Placement::Segment,
        .runCommand = vtextension::CommandPermission::Deny,
    };
};

/// @return Whether @p extension renders through a fresh instance of @p compiled.
[[nodiscard]] bool rendersAfresh(HostileExtension& extension, vtextension::CompiledExtension& compiled)
{
    auto instance = extension.create(compiled);
    return instance && (*instance)->render(vtextension::testing::surface(10, 1)).has_value();
}

/// @return How a render through a fresh instance of @p compiled failed.
[[nodiscard]] std::optional<ExtensionError> renderFailure(HostileExtension& extension,
                                                          vtextension::CompiledExtension& compiled)
{
    auto instance = extension.create(compiled);
    if (!instance)
        return instance.error();
    auto const rendered = (*instance)->render(vtextension::testing::surface(10, 1));
    return rendered ? std::nullopt : std::optional { rendered.error() };
}

} // namespace

TEST_CASE("Hostility.oneCrashIsOneFault", "[vtextension][process][hostility]")
{
    auto const deadline = CaseDeadline { "Hostility.oneCrashIsOneFault" };
    auto extension = HostileExtension { "abort", &withoutHostileMode };
    auto const compiled = extension.acquire();
    REQUIRE(compiled.has_value());
    CHECK(renderFailure(extension, **compiled) == ExtensionError::HostUnavailable);
    CHECK((*compiled)->breaker()->faultCount() == 1);
    CHECK(rendersAfresh(extension, **compiled));
    CHECK(extension.composition().recording().launches() == 2);
}

TEST_CASE("Hostility.oneHangIsOneFault", "[vtextension][process][hostility]")
{
    auto const deadline = CaseDeadline { "Hostility.oneHangIsOneFault" };
    auto extension = HostileExtension { "hang-at-instantiate", &withoutHostileMode };
    auto const compiled = extension.acquire();
    REQUIRE(compiled.has_value());
    auto const instance = extension.create(**compiled);
    REQUIRE_FALSE(instance.has_value());
    CHECK(instance.error() == ExtensionError::HostUnavailable);
    CHECK((*compiled)->breaker()->faultCount() == 1);
    CHECK(rendersAfresh(extension, **compiled));
}

TEST_CASE("Hostility.oneViolationIsOneFault", "[vtextension][process][hostility]")
{
    auto const deadline = CaseDeadline { "Hostility.oneViolationIsOneFault" };
    auto extension = HostileExtension { "garbage", &withoutHostileMode };
    auto const compiled = extension.acquire();
    REQUIRE(compiled.has_value());
    CHECK(renderFailure(extension, **compiled) == ExtensionError::HostUnavailable);
    CHECK((*compiled)->breaker()->faultCount() == 1);
    CHECK(rendersAfresh(extension, **compiled));
}

TEST_CASE("Hostility.repeatedCrashesOpenTheBreaker", "[vtextension][process][hostility]")
{
    // Every helper crashes at its first call: each round is one fault, and the third opens the breaker,
    // which then starts no helper more.
    auto const deadline = CaseDeadline { "Hostility.repeatedCrashesOpenTheBreaker" };
    auto extension = HostileExtension { "abort", {} };
    auto const compiled = extension.acquire();
    REQUIRE(compiled.has_value());
    auto const& breaker = *(*compiled)->breaker();
    for (auto const round: { 1, 2, 3 })
    {
        CHECK(renderFailure(extension, **compiled) == ExtensionError::HostUnavailable);
        CHECK(breaker.faultCount() == static_cast<std::size_t>(round));
    }
    CHECK(breaker.state() == vtextension::BreakerState::Open);
    auto const fourth = extension.create(**compiled);
    REQUIRE_FALSE(fourth.has_value());
    CHECK(fourth.error() == ExtensionError::HostUnavailable);
    CHECK(extension.composition().recording().launches() == 3);
}

TEST_CASE("Hostility.hangingLoadOpensTheBreaker", "[vtextension][process][hostility]")
{
    // Every helper holds its Loaded back: each acquire times out, and counts as a fault of the extension's
    // breaker (D53); once it opened, nothing is loaded, nor a helper started.
    auto const deadline = CaseDeadline { "Hostility.hangingLoadOpensTheBreaker" };
    auto extension = HostileExtension { "hang-at-load", {} };
    for ([[maybe_unused]] auto const round: { 1, 2, 3 })
    {
        auto const acquired = extension.acquire();
        REQUIRE_FALSE(acquired.has_value());
        CHECK(acquired.error() == ExtensionError::HostUnavailable);
    }
    auto const fourth = extension.acquire();
    REQUIRE_FALSE(fourth.has_value());
    CHECK(fourth.error() == ExtensionError::HostUnavailable);
    CHECK(extension.composition().recording().launches() == 3);
}
