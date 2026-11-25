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
#include <ranges>
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
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#ifndef _WIN32
    #include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>
#endif

namespace process = vtextension::wasm::remote::process;
namespace transport = vtextension::wasm::remote::transport;
namespace contract = vtextension::wasm::testing::contract;
using contour::extension_host::testing::GarbageAnswer;
using contour::extension_host::testing::hostileModeNamed;
using contour::extension_host::testing::HostileOption;
using process::KillReason;
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
using vtextension::wasm::remote::HostAttachment;
using vtextension::wasm::remote::HostConnection;
using vtextension::wasm::remote::HostStep;
using vtextension::wasm::remote::IHostConnector;
using vtextension::wasm::remote::IHostHandle;
using vtextension::wasm::remote::StepWatch;
namespace proto = vtextension::wasm::remote::proto;
using namespace std::chrono_literals;

namespace
{

/// The budgets the hostile hosts are held to: short, so that a hang costs little, and far enough apart that
/// the time a hang took names the budget that ended it.
constexpr auto Timeouts =
    HostTimeouts { .load = 700ms, .instantiate = 1100ms, .handshake = 300ms, .interruptGrace = 50ms };

/// How long past its budget a step that overran may take to fail, timed from when its watch was armed: the
/// kill and its observation. The helper's start, and every step before the one that overran, are not timed.
constexpr auto BudgetSlack = 300ms;

/// @return The budget of @p step in Timeouts.
[[nodiscard]] constexpr std::chrono::milliseconds budgetOf(HostStep step) noexcept
{
    switch (step)
    {
        case HostStep::Handshake: return Timeouts.handshake;
        case HostStep::Load: return Timeouts.load;
        case HostStep::Instantiate: return Timeouts.instantiate;
    }
    return {};
}

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

/// When each step's watch was last armed, of every host a StampingConnector started.
class StepStamps
{
  public:
    /// Records that @p step's watch is being armed now.
    void stamp(HostStep step)
    {
        auto const lock = std::scoped_lock { _mutex };
        _armedAt.at(std::to_underlying(step)) = std::chrono::steady_clock::now();
    }

    /// @return When @p step's watch was last armed; nothing if it never was.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> armedAt(HostStep step) const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _armedAt.at(std::to_underlying(step));
    }

  private:
    mutable std::mutex _mutex;
    std::array<std::optional<std::chrono::steady_clock::time_point>, 3> _armedAt {};
};

/// A host handle that stamps each step's watch as it is armed, and otherwise is the handle it wraps.
class StampingHandle final: public IHostHandle
{
  public:
    StampingHandle(std::unique_ptr<IHostHandle> inner, std::shared_ptr<StepStamps> stamps) noexcept:
        _inner { std::move(inner) }, _stamps { std::move(stamps) }
    {
    }

    void interrupt(uint32_t interruptSlot) noexcept override { _inner->interrupt(interruptSlot); }
    void clearInterrupt(uint32_t interruptSlot) noexcept override { _inner->clearInterrupt(interruptSlot); }
    void terminate() noexcept override { _inner->terminate(); }
    void letGo() noexcept override { _inner->letGo(); }
    void attach(HostAttachment attachment) override { _inner->attach(std::move(attachment)); }

    [[nodiscard]] transport::HandleTarget const& handleTarget() const noexcept override
    {
        return _inner->handleTarget();
    }

    [[nodiscard]] StepWatch watch(HostStep step, std::chrono::milliseconds budget) override
    {
        // Stamped before it is armed: the time measured from the stamp is never shorter than the budget ran.
        _stamps->stamp(step);
        return _inner->watch(step, budget);
    }

  private:
    std::unique_ptr<IHostHandle> _inner;
    std::shared_ptr<StepStamps> _stamps;
};

/// A connector whose hosts' handles stamp each step's watch as it is armed (StampingHandle): so that how long
/// a step that overran took is timed from its own start, not from the helper's.
class StampingConnector final: public IHostConnector
{
  public:
    StampingConnector(std::shared_ptr<IHostConnector> inner, std::shared_ptr<StepStamps> stamps) noexcept:
        _inner { std::move(inner) }, _stamps { std::move(stamps) }
    {
    }

    [[nodiscard]] std::expected<HostConnection, ExtensionError> connect() override
    {
        return _inner->connect().transform([this](HostConnection connection) {
            connection.handle = std::make_unique<StampingHandle>(std::move(connection.handle), _stamps);
            return connection;
        });
    }

    void refused(vtextension::wasm::remote::HostRefusal refusal) override { _inner->refused(refusal); }

  private:
    std::shared_ptr<IHostConnector> _inner;
    std::shared_ptr<StepStamps> _stamps;
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

/// @return A remote backend over @p composition whose hosts are the helper @p config names, each step's watch
///         stamped into @p stamps as it is armed.
[[nodiscard]] std::unique_ptr<IWasmBackend> stampedBackend(HelperComposition const& composition,
                                                           HelperBackendConfig const& config,
                                                           std::shared_ptr<StepStamps> stamps)
{
    return vtextension::wasm::remote::makeRemoteBackend(
        composition.kind(),
        vtextension::wasm::BackendConfig { .timeouts = config.timeouts,
                                           .connector = std::make_shared<StampingConnector>(
                                               composition.connector(config), std::move(stamps)),
                                           .lockdown = config.lockdown });
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
    Add,          ///< `call("add", {2, 3})`, under the watchdog.
    PingThenAdd,  ///< `call("ping-then-add", {2, 3})`, a host call first, under the watchdog.
    Read,         ///< `memory(16, 5)`, under the watchdog.
    Load,         ///< `runtime->load(...)`.
    Instantiate,  ///< `module->instantiate(...)`.
    ReleaseFlood, ///< FloodInstances instances dropped at once, then `module->instantiate(...)`.
    Grow,         ///< `call("grow")`: its memory grows by 4 pages, under the watchdog.
};

/// How many instances Operation::ReleaseFlood drops.
constexpr auto FloodInstances = 1000;

/// The limits of the runtime Operation::ReleaseFlood floods: memory of one page, so that the memory objects
/// of FloodInstances live instances stay well within a host's cap on them.
constexpr auto FloodLimits =
    vtextension::wasm::RuntimeLimits { .memoryBytes = proto::WasmPageBytes,
                                       .stackBytes = contract::Limits.stackBytes,
                                       .tableElements = contract::Limits.tableElements };

/// How long dropping one instance may take: it never waits for the host.
constexpr auto DropBound = 100ms;

/// How an operation failed, of the ways that matter here.
enum class Failure : uint8_t
{
    None,            ///< It did not.
    CallCrashed,     ///< CallFailure::HostCrashed.
    ReadCrashed,     ///< MemoryFailure::HostCrashed.
    HostUnavailable, ///< ExtensionError::HostUnavailable.
    Other,           ///< Any other way.
};

/// How the hostile helper ended, and so which kill the supervisor counted for it.
enum class HelperEnd : uint8_t
{
    /// Contour found it broke the protocol and stopped it: one kill Requested, no unclean exit.
    Refused,
    /// An interrupted call of its did not end within the grace: one Grace kill, no unclean exit.
    GraceExpired,
    /// It overstayed a step's budget: one StepTimeout kill, no unclean exit.
    StepOverran,
    /// It crashed or exited by itself: no kill for a timeout or a violation, and one unclean exit -- a
    /// non-zero exit or a signal the supervisor did not send. Contour, seeing the channels end, may still
    /// ask for a kill before the end was reaped: at most one kill Requested, which does nothing.
    OnItsOwn,
};

/// Whether the connector starts another host after a hostile one.
enum class NextHost : uint8_t
{
    NoneStarted, ///< None: the hostile one was refused for a reason every host would share.
    StartedAnew, ///< One that plays by the rules.
};

/// One hostile mode, what Contour asks of it, and what that must come to.
struct HostileRow
{
    std::string_view mode;
    Operation operation;
    Failure failure;
    HelperEnd end;
    std::optional<HostStep> step {}; ///< The step whose budget ended it; nothing unless StepOverran.
    NextHost next = NextHost::StartedAnew;
};

constexpr auto HostileRows = std::array {
    HostileRow { "abort", Operation::Add, Failure::CallCrashed, HelperEnd::OnItsOwn },
    HostileRow { "segfault", Operation::Add, Failure::CallCrashed, HelperEnd::OnItsOwn },
    HostileRow { "garbage", Operation::Add, Failure::CallCrashed, HelperEnd::Refused },
    HostileRow { "stop-reading", Operation::Add, Failure::CallCrashed, HelperEnd::GraceExpired },
    HostileRow { "exit-in-callhost", Operation::PingThenAdd, Failure::CallCrashed, HelperEnd::OnItsOwn },
    HostileRow { "over-cap", Operation::Read, Failure::ReadCrashed, HelperEnd::Refused },
    HostileRow { "bogus-size", Operation::Read, Failure::ReadCrashed, HelperEnd::Refused },
    HostileRow { "stall-after-header", Operation::Read, Failure::ReadCrashed, HelperEnd::GraceExpired },
    HostileRow { "exit-before-hello", Operation::Load, Failure::HostUnavailable, HelperEnd::OnItsOwn },
    // Another protocol version stays installed: no host is started again.
    HostileRow { "wrong-version",
                 Operation::Load,
                 Failure::HostUnavailable,
                 HelperEnd::Refused,
                 std::nullopt,
                 NextHost::NoneStarted },
    HostileRow { "hang-at-handshake",
                 Operation::Load,
                 Failure::HostUnavailable,
                 HelperEnd::StepOverran,
                 HostStep::Handshake },
    HostileRow { "hang-at-create-runtime",
                 Operation::Load,
                 Failure::HostUnavailable,
                 HelperEnd::StepOverran,
                 HostStep::Handshake },
    HostileRow {
        "hang-at-load", Operation::Load, Failure::HostUnavailable, HelperEnd::StepOverran, HostStep::Load },
    HostileRow { "hang-at-instantiate",
                 Operation::Instantiate,
                 Failure::HostUnavailable,
                 HelperEnd::StepOverran,
                 HostStep::Instantiate },
    HostileRow { "grow-past-cap", Operation::Grow, Failure::CallCrashed, HelperEnd::Refused },
    HostileRow { "grow-shrinks", Operation::Grow, Failure::CallCrashed, HelperEnd::Refused },
    HostileRow { "grow-unaligned", Operation::Grow, Failure::CallCrashed, HelperEnd::Refused },
    HostileRow { "grow-wrong-instance", Operation::Grow, Failure::CallCrashed, HelperEnd::Refused },
    // Granted memory it cannot open ends the host, as a crash of its own: not a violation Contour refused.
    HostileRow { "memory-unusable", Operation::Grow, Failure::CallCrashed, HelperEnd::OnItsOwn },
    HostileRow { "stop-reading-at-release",
                 Operation::ReleaseFlood,
                 Failure::HostUnavailable,
                 HelperEnd::StepOverran,
                 HostStep::Instantiate },
};

/// @return The kill @p end is counted as; nothing for a helper that ended by itself.
[[nodiscard]] constexpr std::optional<KillReason> killOf(HelperEnd end) noexcept
{
    switch (end)
    {
        case HelperEnd::Refused: return KillReason::Requested;
        case HelperEnd::GraceExpired: return KillReason::Grace;
        case HelperEnd::StepOverran: return KillReason::StepTimeout;
        case HelperEnd::OnItsOwn: return std::nullopt;
    }
    return std::nullopt;
}

/// Every reason the supervisor kills for.
constexpr auto KillReasons = std::array {
    KillReason::Requested, KillReason::StepTimeout, KillReason::Grace,
    KillReason::Dropped,   KillReason::Shutdown,
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

/// What an operation came to.
struct Outcome
{
    Failure failure = Failure::Other;               ///< How its failing step failed.
    std::chrono::nanoseconds elapsed {};            ///< How long that step took.
    std::chrono::steady_clock::time_point ended {}; ///< When it ended.
    std::shared_ptr<IWasmModule> module = nullptr;  ///< What it leaves to recover with; none after a load.
};

/// How an operation's step failed, and when.
struct Timed
{
    Failure failure = Failure::Other;               ///< How it failed.
    std::chrono::nanoseconds elapsed {};            ///< How long it took.
    std::chrono::steady_clock::time_point ended {}; ///< When it ended.
};

/// @return How @p step failed, how long it took, and when it ended.
template <typename Step>
[[nodiscard]] Timed timed(Step const& step)
{
    auto const began = std::chrono::steady_clock::now();
    auto const failure = step();
    auto const ended = std::chrono::steady_clock::now();
    return { .failure = failure, .elapsed = ended - began, .ended = ended };
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

/// Drops FloodInstances instances of @p module, each within DropBound, with a host that reads none of the
/// releases: none may wait for it.
void flood(IWasmModule& module, contract::RecordingHostContext& context)
{
    auto instances = std::vector<std::unique_ptr<IWasmInstance>> {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, FloodInstances))
        instances.push_back(contract::instantiate(module, context));
    auto slowest = std::chrono::nanoseconds {};
    for (auto& instance: instances)
    {
        auto const began = std::chrono::steady_clock::now();
        instance.reset();
        slowest = std::max(slowest, std::chrono::nanoseconds { std::chrono::steady_clock::now() - began });
    }
    CHECK(slowest < DropBound);
}

/// Runs @p row's operation on @p runtime, whose backend is @p backend.
/// @return What it came to.
[[nodiscard]] Outcome operate(HostileRow const& row, IWasmBackend& backend, IWasmRuntime& runtime)
{
    if (row.operation == Operation::Load)
    {
        auto const [failure, elapsed, ended] = timed([&] { return failureOf(loadInstanceModule(runtime)); });
        return Outcome { .failure = failure, .elapsed = elapsed, .ended = ended, .module = nullptr };
    }
    auto loaded = loadInstanceModule(runtime);
    REQUIRE(loaded.has_value());
    auto const module = *std::move(loaded);
    auto context = contract::RecordingHostContext {};
    auto const instantiating = [&] {
        return failureOf(module->instantiate(context));
    };
    if (row.operation == Operation::Instantiate)
    {
        auto const [failure, elapsed, ended] = timed(instantiating);
        return Outcome { .failure = failure, .elapsed = elapsed, .ended = ended, .module = module };
    }
    if (row.operation == Operation::ReleaseFlood)
    {
        flood(*module, context);
        auto const [failure, elapsed, ended] = timed(instantiating);
        return Outcome { .failure = failure, .elapsed = elapsed, .ended = ended, .module = module };
    }
    auto const instance = contract::instantiate(*module, context);
    auto const [failure, elapsed, ended] = timed([&] {
        switch (row.operation)
        {
            case Operation::Add: return callUnderWatchdog(backend, *instance, "add");
            case Operation::PingThenAdd: return callUnderWatchdog(backend, *instance, "ping-then-add");
            case Operation::Grow: return callUnderWatchdog(backend, *instance, "grow");
            case Operation::Read:
                return contract::onPreparedThread(backend,
                                                  [&] {
                                                      auto const watchdog = Watchdog { *instance };
                                                      return failureOf(instance->memory(16, 5));
                                                  })
                    .value_or(Failure::Other);
            case Operation::Load:
            case Operation::Instantiate:
            case Operation::ReleaseFlood: break;
        }
        return Failure::Other;
    });
    return Outcome { .failure = failure, .elapsed = elapsed, .ended = ended, .module = module };
}

/// Checks the kills @p composition's supervisor counted for a helper that ended as @p end.
void checkKills(HelperComposition const& composition, HelperEnd end)
{
    auto const expected = killOf(end);
    for (auto const reason: KillReasons)
    {
        auto const kills = composition.killsFor(reason);
        CAPTURE(std::to_underlying(reason));
        if (expected)
            CHECK(kills == (reason == *expected ? 1U : 0U));
        else if (reason == KillReason::Requested)
            CHECK(kills <= 1U);
        else
            CHECK(kills == 0U);
    }
}

} // namespace

TEST_CASE("Hostility.garbageIsAViolationAtOnce", "[vtextension][hostility]")
{
    // The garbage a hostile host answers is refused by its header, not awaited as a frame still arriving.
    auto const size = proto::frameSizeOf(
        GarbageAnswer, proto::Direction::ToParent, proto::ChannelKind::Lane, proto::PduLimits {});
    REQUIRE_FALSE(size.has_value());
    CHECK(size.error() != proto::DecodeError::NeedMoreData);
}

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
                auto const stamps = std::make_shared<StepStamps>();
                auto const backend = stampedBackend(composition, hostileHelper(row.mode), stamps);
                auto const runtime = contract::createRuntime(
                    *backend, row.operation == Operation::ReleaseFlood ? FloodLimits : contract::Limits);

                auto outcome = operate(row, *backend, *runtime);
                CHECK(outcome.failure == row.failure);
                CHECK(outcome.elapsed < FailureBound);
                if (row.step)
                {
                    // The budget of the step that overran, and no other, ended it: timed from when its watch
                    // was armed, so that neither the helper's start nor the steps before count.
                    auto const armedAt = stamps->armedAt(*row.step);
                    REQUIRE(armedAt.has_value());
                    auto const stepTook = outcome.ended - *armedAt;
                    CHECK(stepTook >= budgetOf(*row.step));
                    CHECK(stepTook < budgetOf(*row.step) + BudgetSlack);
                }

                // One death, whether the helper died on its own or was killed: a kill is reaped and observed
                // like any end. The kill names how it ended.
                CHECK(awaitDeaths(composition, 1));
                composition.supervisor().sync();
                CHECK(composition.supervisor().counters().deathsObserved == 1);
                CHECK(composition.recording().launches() == 1);
                CHECK(composition.supervisor().counters().uncleanExits == expectedUnclean);
                checkKills(composition, row.end);

                if (row.next == NextHost::NoneStarted)
                {
                    // Refused for good: the next operation starts no host.
                    auto const loaded = loadInstanceModule(*runtime);
                    REQUIRE_FALSE(loaded.has_value());
                    CHECK(loaded.error() == ExtensionError::HostUnavailable);
                    CHECK(composition.recording().launches() == 1);
                }
                else
                {
                    // The next operation runs in a host started anew, which plays by the rules.
                    if (!outcome.module)
                    {
                        auto loaded = loadInstanceModule(*runtime);
                        REQUIRE(loaded.has_value());
                        outcome.module = *std::move(loaded);
                    }
                    CHECK(addsOnAFreshInstance(*backend, *outcome.module));
                    CHECK(composition.recording().launches() == 2);
                }
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
    /// @param composition What starts the helpers; must outlive the extension.
    /// @param mode The hostile mode of every helper started.
    HostileExtension(HelperComposition const& composition, std::string_view mode):
        _backend { composition.backend(hostileHelper(mode)) },
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

  private:
    static constexpr auto Limits = vtextension::ExtensionLimits {};

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

/// Checks that every helper of @p composition ended once the case let go of it, @p unclean of them
/// uncleanly: those that crashed, and no other.
void checkEveryHelperEnded(HelperComposition const& composition, std::size_t unclean)
{
    CHECK(composition.awaitEveryHelperReaped());
    CHECK(composition.supervisor().counters().uncleanExits == unclean);
}

} // namespace

TEST_CASE("Hostility.oneCrashIsOneFault", "[vtextension][process][hostility]")
{
    auto const deadline = CaseDeadline { "Hostility.oneCrashIsOneFault" };
    auto const composition = HelperComposition { HelperTeardownGrace, &withoutHostileMode };
    {
        auto extension = HostileExtension { composition, "abort" };
        auto const compiled = extension.acquire();
        REQUIRE(compiled.has_value());
        CHECK(renderFailure(extension, **compiled) == ExtensionError::HostUnavailable);
        CHECK((*compiled)->breaker()->faultCount() == 1);
        CHECK(rendersAfresh(extension, **compiled));
        CHECK(composition.recording().launches() == 2);
    }
    checkEveryHelperEnded(composition, 1);
}

TEST_CASE("Hostility.oneHangIsOneFault", "[vtextension][process][hostility]")
{
    auto const deadline = CaseDeadline { "Hostility.oneHangIsOneFault" };
    auto const composition = HelperComposition { HelperTeardownGrace, &withoutHostileMode };
    {
        auto extension = HostileExtension { composition, "hang-at-instantiate" };
        auto const compiled = extension.acquire();
        REQUIRE(compiled.has_value());
        auto const instance = extension.create(**compiled);
        REQUIRE_FALSE(instance.has_value());
        CHECK(instance.error() == ExtensionError::HostUnavailable);
        CHECK((*compiled)->breaker()->faultCount() == 1);
        CHECK(rendersAfresh(extension, **compiled));
    }
    checkEveryHelperEnded(composition, 0);
}

TEST_CASE("Hostility.oneViolationIsOneFault", "[vtextension][process][hostility]")
{
    auto const deadline = CaseDeadline { "Hostility.oneViolationIsOneFault" };
    auto const composition = HelperComposition { HelperTeardownGrace, &withoutHostileMode };
    {
        auto extension = HostileExtension { composition, "garbage" };
        auto const compiled = extension.acquire();
        REQUIRE(compiled.has_value());
        CHECK(renderFailure(extension, **compiled) == ExtensionError::HostUnavailable);
        CHECK((*compiled)->breaker()->faultCount() == 1);
        CHECK(rendersAfresh(extension, **compiled));
    }
    checkEveryHelperEnded(composition, 0);
}

TEST_CASE("Hostility.repeatedCrashesOpenTheBreaker", "[vtextension][process][hostility]")
{
    // Every helper crashes at its first call: each round is one fault, and the third opens the breaker,
    // which then starts no helper more.
    auto const deadline = CaseDeadline { "Hostility.repeatedCrashesOpenTheBreaker" };
    auto const composition = HelperComposition {};
    {
        auto extension = HostileExtension { composition, "abort" };
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
        CHECK(composition.recording().launches() == 3);
    }
    checkEveryHelperEnded(composition, 3);
}

TEST_CASE("Hostility.hangingLoadOpensTheBreaker", "[vtextension][process][hostility]")
{
    // Every helper holds its Loaded back: each acquire times out, and a load that fails for its host counts
    // as a fault of the extension's breaker like a host that dies under an instance; once it opened, nothing
    // is loaded, nor a helper started.
    auto const deadline = CaseDeadline { "Hostility.hangingLoadOpensTheBreaker" };
    auto const composition = HelperComposition {};
    {
        auto extension = HostileExtension { composition, "hang-at-load" };
        for ([[maybe_unused]] auto const round: { 1, 2, 3 })
        {
            auto const acquired = extension.acquire();
            REQUIRE_FALSE(acquired.has_value());
            CHECK(acquired.error() == ExtensionError::HostUnavailable);
        }
        auto const fourth = extension.acquire();
        REQUIRE_FALSE(fourth.has_value());
        CHECK(fourth.error() == ExtensionError::HostUnavailable);
        CHECK(composition.recording().launches() == 3);
    }
    checkEveryHelperEnded(composition, 0);
}

namespace
{

/// How long the renders through a host that scribbles on memory may take, all of them.
constexpr auto ScribbledRendersBound = std::chrono::seconds { 10 };

/// How many renders run through a host that scribbles on memory.
constexpr auto ScribbledRenders = 200;

} // namespace

#ifndef _WIN32
TEST_CASE("Hostility.descriptorUpIsNeverInstalled", "[vtextension][process][hostility]")
{
    // The host attaches a descriptor to its CallResult. Where Contour's ends refuse descriptors (Linux 6.16+)
    // its send fails and it passes the frame on bare; elsewhere Contour's plain receives leave the system to
    // drop it. Either way the call is answered as if nothing came with it, and the host serves on.
    auto const deadline = CaseDeadline { "Hostility.descriptorUpIsNeverInstalled" };
    auto const composition = HelperComposition { HelperTeardownGrace, &withoutHostileMode };
    {
        auto const backend = composition.backend(hostileHelper("descriptor-up"));
        auto const runtime = contract::createRuntime(*backend);
        auto loaded = loadInstanceModule(*runtime);
        REQUIRE(loaded.has_value());
        auto const module = *std::move(loaded);
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(*module, context);
        auto const before = vtextension::wasm::remote::transport::testing::openDescriptorCount();
        auto const sum = contract::onPreparedThread(*backend, [&]() -> std::optional<uint32_t> {
            auto argv = std::array<uint32_t, 2> { 2, 3 };
            if (!instance->call(contract::functionOf(*instance, "add"), argv))
                return std::nullopt;
            return argv[0];
        });
        CHECK(sum == std::optional { std::optional { uint32_t { 5 } } });
        CHECK(vtextension::wasm::remote::transport::testing::openDescriptorCount() == before);
        composition.supervisor().sync();
        CHECK(composition.supervisor().counters().deathsObserved == 0);
        CHECK(composition.recording().launches() == 1);
    }
    checkEveryHelperEnded(composition, 0);
}
#endif

TEST_CASE("hostile memory through a real host", "[vtextension][process][hostility]")
{
    // A host that flips the bytes of every memory Contour grants it, all the time: renders succeed, or fail
    // with an error of the extension's; none ends Contour, nor waits past the deadline.
    auto const deadline = CaseDeadline { "hostile memory through a real host" };
    auto const composition = HelperComposition { HelperTeardownGrace, &withoutHostileMode };
    {
        auto extension = HostileExtension { composition, "scribble-memory" };
        auto const began = std::chrono::steady_clock::now();
        auto rendered = 0;
        auto failed = 0;
        for ([[maybe_unused]] auto const render: std::views::iota(0, ScribbledRenders))
        {
            auto const compiled = extension.acquire();
            auto const failure =
                compiled ? renderFailure(extension, **compiled) : std::optional { compiled.error() };
            ++(failure ? failed : rendered);
        }
        INFO("rendered " << rendered << ", failed " << failed);
        CHECK(rendered + failed == ScribbledRenders);
        CHECK(std::chrono::steady_clock::now() - began < ScribbledRendersBound);
    }
    checkEveryHelperEnded(composition, 0);
}

TEST_CASE("Hostility.copyingHostServes", "[vtextension][process][hostility]")
{
    // A host that copies memory where the system could map it: its Hello says so, and Contour reaches memory
    // through the protocol.
    auto const deadline = CaseDeadline { "Hostility.copyingHostServes" };
    auto const composition = HelperComposition { HelperTeardownGrace, &withoutHostileMode };
    {
        auto const backend = composition.backend(hostileHelper("copy-memory"));
        auto const runtime = contract::createRuntime(*backend);
        auto loaded = loadInstanceModule(*runtime);
        REQUIRE(loaded.has_value());
        CHECK(addsOnAFreshInstance(*backend, **loaded));
        auto const read = contract::onPreparedThread(*backend, [&]() -> std::string {
            auto context = contract::RecordingHostContext {};
            auto const instance = (*loaded)->instantiate(context);
            if (!instance)
                return {};
            auto const view = (*instance)->memory(16, 5);
            if (!view)
                return {};
            auto text = std::string {};
            for (auto const byte: *view)
                text.push_back(static_cast<char>(byte));
            return text;
        });
        // The "hello" the module's data puts at 16, read through the protocol.
        CHECK(read == std::optional { std::string { "hello" } });
        CHECK(composition.recording().launches() == 1);
    }
    checkEveryHelperEnded(composition, 0);
}
