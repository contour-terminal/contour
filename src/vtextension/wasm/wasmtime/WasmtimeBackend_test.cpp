// SPDX-License-Identifier: Apache-2.0
//
// The Wasmtime backend: the backend contract every runtime must meet, and what is Wasmtime's alone,
// the features it enables unless told otherwise, checked with its C API directly.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <wasmtime.h>

#ifndef _WIN32
    #include <csignal>
#endif

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/EngineFeatures.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/FeatureModules.hpp>
#include <vtextension/wasm/testing/RenderMeasurement.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeBackend.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeRuntime.hpp>

using vtextension::ExtensionError;
using vtextension::wasm::FeatureState;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::MemoryGrowthReporting;
using vtextension::wasm::RuntimeLimits;
using vtextension::wasm::wasmtime::KindState;
using vtextension::wasm::wasmtime::makeKindState;
using vtextension::wasm::wasmtime::makeWasmtimeBackend;
using vtextension::wasm::wasmtime::TrapMode;
using vtextension::wasm::wasmtime::WasmtimeConfig;

namespace
{

/// Makes the backend under test, for the contract suite.
struct WasmtimeTraits
{
    /// @return A Wasmtime backend of the one kind state this test binary shares, as a process would.
    [[nodiscard]] static std::unique_ptr<IWasmBackend> make()
    {
        return makeWasmtimeBackend(kind(), WasmtimeConfig { .trapMode = TrapMode::Software });
    }

    /// @return The one kind state this test binary shares.
    [[nodiscard]] static std::shared_ptr<KindState> const& kind()
    {
        static auto const state = makeKindState();
        return state;
    }
};

/// @return Whether an engine left at Wasmtime's feature defaults compiles @p bytes. Its traps are checked
///         in software all the same: an engine with signal-based traps would install Wasmtime's handlers
///         for the rest of this process, over the sanitizer's, and blind the signal-handler case.
[[nodiscard]] bool defaultEngineAccepts(std::span<std::uint8_t const> bytes)
{
    namespace detail = vtextension::wasm::wasmtime::detail;
    auto* const config = wasm_config_new();
    wasmtime_config_signals_based_traps_set(config, false);
    wasmtime_config_macos_use_mach_ports_set(config, false);
    // Compiled on this thread, as the backend's engines compile: Wasmtime's compilation threads
    // synchronize in Rust no sanitizer sees. What the engine accepts does not depend on it.
    wasmtime_config_parallel_compilation_set(config, false);
    auto const engine = detail::EngineHandle { wasm_engine_new_with_config(config) };
    REQUIRE(engine);
    auto* module = static_cast<wasmtime_module_t*>(nullptr);
    auto const error =
        detail::ErrorHandle { wasmtime_module_new(engine.get(), bytes.data(), bytes.size(), &module) };
    auto const owned = detail::ModuleHandle { module };
    return !error;
}

/// @return The module the WebAssembly text @p wat assembles to.
[[nodiscard]] std::vector<std::byte> assemble(std::string_view wat)
{
    namespace detail = vtextension::wasm::wasmtime::detail;
    auto bytes = wasm_byte_vec_t {};
    auto const error = detail::ErrorHandle { wasmtime_wat2wasm(wat.data(), wat.size(), &bytes) };
    REQUIRE_FALSE(error);
    auto const view = std::as_bytes(std::span { bytes.data, bytes.size });
    auto module = std::vector<std::byte> { view.begin(), view.end() };
    wasm_byte_vec_delete(&bytes);
    return module;
}

/// @return A module importing `arity` `f`, taking @p arity i32s, and exporting `call`, which calls it with
///         1, 2, ... @p arity.
[[nodiscard]] std::vector<std::byte> arityModule(uint32_t arity)
{
    auto parameters = std::string {};
    auto arguments = std::string {};
    for (auto const value: std::views::iota(uint32_t { 1 }, arity + 1))
    {
        parameters += " i32";
        arguments += std::format(" (i32.const {})", value);
    }
    return assemble(std::format(R"((module
  (import "arity" "f" (func $f (param{})))
  (func (export "call") (call $f{})))
)",
                                parameters,
                                arguments));
}

/// A host context whose every host function call calls back into its instance first, as the extension
/// host does to allocate in guest memory, and then fails.
class ReenteringHostContext final: public vtextension::wasm::IHostContext
{
  public:
    /// The instance to call back into; set once it exists.
    vtextension::wasm::IWasmInstance* instance = nullptr;
    /// What the call back into the instance returned.
    std::optional<std::expected<uint32_t, vtextension::wasm::CallFailure>> reentered;

    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        auto argv = std::array<uint32_t, 2> { 2, 3 };
        auto const add = instance->function("add");
        reentered = instance->call(*add, argv).transform([&] { return argv[0]; });
        return std::unexpected(ExtensionError::PermissionDenied);
    }
};

/// `count` counts to a million and returns the count; `ping-then-count` calls `ping(0)` first.
constexpr auto CountingModule = std::string_view { R"((module
  (import "contract:test/host@0" "ping" (func $ping (param i32)))
  (func $count (export "count") (result i32) (local $n i32)
    (loop $again
      (local.set $n (i32.add (local.get $n) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $n) (i32.const 1000000))))
    (local.get $n))
  (func (export "ping-then-count") (result i32) (call $ping (i32.const 0)) (call $count)))
)" };

/// What `count` of CountingModule returns.
constexpr auto CountedTo = uint32_t { 1'000'000 };

/// A host context whose every host function call interrupts its own instance, calls back into it, and
/// absorbs the failure, as a host function may when it can do without what it asked for.
class SelfInterruptingHostContext final: public vtextension::wasm::IHostContext
{
  public:
    /// Names the instance: it is created with this context, so the two are wired after both exist.
    /// @param instance The instance; outlives every call made through this context.
    void attach(vtextension::wasm::IWasmInstance& instance) noexcept { _instance = &instance; }

    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        _instance->interrupt();
        if (auto const add = _instance->function("add"))
        {
            auto argv = std::array<uint32_t, 2> { 2, 3 };
            _nested = _instance->call(*add, argv);
        }
        return {};
    }

    /// @return What the call back into the instance returned; nothing before it was made.
    [[nodiscard]] std::optional<std::expected<void, vtextension::wasm::CallFailure>> const& nested()
        const noexcept
    {
        return _nested;
    }

  private:
    vtextension::wasm::IWasmInstance* _instance = nullptr;
    std::optional<std::expected<void, vtextension::wasm::CallFailure>> _nested;
};

/// A host context whose every host function call interrupts another instance: from inside a running
/// call, at a known point of it.
class InterruptingHostContext final: public vtextension::wasm::IHostContext
{
  public:
    /// @param target The instance every host function call interrupts; outlives the context's use.
    explicit InterruptingHostContext(vtextension::wasm::IWasmInstance& target) noexcept: _target { &target }
    {
    }

    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        _target->interrupt();
        return {};
    }

  private:
    vtextension::wasm::IWasmInstance* _target;
};

} // namespace

CONTOUR_WASM_BACKEND_CONTRACT(WasmtimeTraits)

TEST_CASE("WasmtimeBackend.hostFunctionArities", "[wasmtime]")
{
    // A host function takes from none up to MaxHostParameters i32s, each reaching the host in order. One
    // taking more cannot be provided, and the runtime is refused.
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::HostFunction;
    using vtextension::wasm::wasmtime::MaxHostParameters;
    auto const backend = WasmtimeTraits::make();

    for (auto const arity: { uint32_t { 0 }, uint32_t { 1 }, MaxHostParameters })
    {
        INFO("arity " << arity);
        auto const hostFunctions =
            std::array { HostFunction { .module = "arity", .name = "f", .parameterCount = arity } };
        auto const runtime = backend->createRuntime(contract::Limits, hostFunctions);
        REQUIRE(runtime.has_value());
        auto const guard = contract::enterThread(*backend);
        auto const module = (*runtime)->load(arityModule(arity));
        REQUIRE(module.has_value());
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(**module, context);

        CHECK(instance->call(contract::functionOf(*instance, "call"), {}).has_value());
        auto const expected = std::ranges::to<std::vector>(std::views::iota(uint32_t { 1 }, arity + 1));
        CHECK(context.calls()
              == std::vector { contract::HostCall {
                  .functionIndex = 0, .arguments = expected, .thread = std::this_thread::get_id() } });
    }

    auto const tooMany = std::array { HostFunction {
        .module = "arity", .name = "f", .parameterCount = MaxHostParameters + 1 } };
    auto const refused = backend->createRuntime(contract::Limits, tooMany);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::RuntimeUnavailable);
}

TEST_CASE("WasmtimeBackend.hostFunctionCallsBackIn", "[wasmtime]")
{
    // A host function calling back into its instance, as the extension host does to allocate in guest
    // memory, gets its answer from the same store; its own failure after that still fails the outer call
    // as a host failure.
    namespace contract = vtextension::wasm::testing::contract;
    auto const backend = WasmtimeTraits::make();
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = ReenteringHostContext {};
    auto const instance = contract::instantiate(*module, context);
    context.instance = instance.get();

    auto const called = instance->call(contract::functionOf(*instance, "ping-then-store"), {});
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error() == vtextension::wasm::CallFailure::HostFailure);
    CHECK(context.reentered == std::expected<uint32_t, vtextension::wasm::CallFailure> { 5 });
    CHECK(contract::wordAt(*instance, 0) == 0);
}

TEST_CASE("WasmtimeBackend.callsWideExports", "[wasmtime]")
{
    // An export taking more values than a call passes without allocating still receives every one, in
    // order: `sum` returns the first of 20 parameters plus twice the second, and so on.
    namespace contract = vtextension::wasm::testing::contract;
    constexpr auto Width = uint32_t { 20 };
    auto parameters = std::string {};
    auto body = std::string { "(i32.const 0)" };
    for (auto const index: std::views::iota(uint32_t { 0 }, Width))
    {
        parameters += " i32";
        body = std::format("(i32.add {} (i32.mul (local.get {}) (i32.const {})))", body, index, index + 1);
    }
    auto const backend = WasmtimeTraits::make();
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = runtime->load(assemble(
        std::format(R"((module (func (export "sum") (param{}) (result i32) {})))", parameters, body)));
    REQUIRE(module.has_value());
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(**module, context);

    // Passed 1, 2, ... 20: 1² + 2² + ... + 20², which no other order of them gives.
    auto argv = std::ranges::to<std::vector>(std::views::iota(uint32_t { 1 }, Width + 1));
    REQUIRE(instance->call(contract::functionOf(*instance, "sum"), argv).has_value());
    CHECK(argv[0] == 2870);
}

TEST_CASE("WasmtimeBackend.refusesWhatWasmtimeEnablesByDefault", "[wasmtime]")
{
    // A feature Wasmtime enables unless told otherwise is refused only because the backend switched it
    // off. Each such module is checked against both: a default engine accepts it, the backend refuses it.
#ifndef _WIN32
    struct sigaction before {};
    sigaction(SIGSEGV, nullptr, &before);
#endif
    auto const backend = WasmtimeTraits::make();
    auto const runtime = vtextension::wasm::testing::contract::createRuntime(*backend);
    auto enabledByDefault = std::size_t { 0 };
    for (auto const& [feature, bytes]: vtextension::wasm::testing::FeatureModules)
    {
        auto const& row = vtextension::wasm::featureRow(feature);
        if (row.state != FeatureState::Off || !defaultEngineAccepts(bytes))
            continue;
        ++enabledByDefault;
        INFO("feature: " << row.name);
        auto const module = std::as_bytes(bytes);
        auto const loaded = runtime->load({ module.begin(), module.end() });
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ExtensionError::InvalidComponent);
    }
    // Wasmtime 48 enables GC, exceptions, 64-bit memories, typed function references, threads, tail
    // calls, multiple memories, SIMD and relaxed SIMD by default; were none of them, this case checked
    // nothing.
    CHECK(enabledByDefault >= 9);
#ifndef _WIN32
    // Neither the backend's engines nor the default ones compared against took the process's SIGSEGV.
    struct sigaction after {};
    sigaction(SIGSEGV, nullptr, &after);
    CHECK(after.sa_sigaction == before.sa_sigaction);
#endif
}

TEST_CASE("WasmtimeBackend.refusesAZeroStack", "[wasmtime]")
{
    // Wasmtime aborts the process on an engine whose stack cap is zero; the backend refuses it first.
    auto const runtime = WasmtimeTraits::make()->createRuntime(
        RuntimeLimits { .memoryBytes = vtextension::wasm::testing::contract::Limits.memoryBytes,
                        .stackBytes = 0,
                        .tableElements = vtextension::wasm::testing::contract::Limits.tableElements },
        vtextension::wasm::testing::contract::HostFunctions);
    REQUIRE_FALSE(runtime.has_value());
    CHECK(runtime.error() == ExtensionError::RuntimeUnavailable);
}

TEST_CASE("WasmtimeBackend.leavesMemoryGrowthUnreported", "[wasmtime]")
{
    CHECK(WasmtimeTraits::make()->capabilities().memoryGrowth == MemoryGrowthReporting::Unreported);
}

TEST_CASE("WasmtimeBackend.wasiReadsTheConfiguredEnvironment", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    // A clock reading 7 seconds past its id, and random bytes that are all 0x5A.
    auto config = WasmtimeConfig {
        .trapMode = TrapMode::Software,
        .wasiEnvironment = {
            .clock = [](uint32_t clockId) { return 7'000'000'000 + uint64_t { clockId }; },
            .random = [](std::span<std::byte> bytes) -> std::expected<void, vtextension::wasm::wasi::Errno> {
                std::ranges::fill(bytes, std::byte { 0x5A });
                return {};
            } },
    };
    auto const backend = makeWasmtimeBackend(WasmtimeTraits::kind(), std::move(config));
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::EveryWasiFunctionModule);
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(*module, context);

    // Both write at address 64: clock_time_get reads the realtime clock (id 0), random_get fills 16 bytes.
    auto error = std::array<uint32_t, 1> {};
    REQUIRE(instance->call(contract::functionOf(*instance, "clock_time_get"), error).has_value());
    CHECK(error[0] == 0);
    auto const time = instance->memory(64, 8);
    REQUIRE(time.has_value());
    auto nanoseconds = uint64_t { 0 };
    for (auto const byte: *time | std::views::reverse)
        nanoseconds = (nanoseconds << 8U) | std::to_integer<uint64_t>(byte);
    CHECK(nanoseconds == 7'000'000'000);

    REQUIRE(instance->call(contract::functionOf(*instance, "random_get"), error).has_value());
    CHECK(error[0] == 0);
    auto const random = instance->memory(64, 16);
    REQUIRE(random.has_value());
    CHECK(std::ranges::all_of(*random, [](std::byte byte) { return byte == std::byte { 0x5A }; }));
}

TEST_CASE("Wasmtime.idleInstanceNeverWakes", "[wasmtime]")
{
    // Nothing but an interrupt moves the epoch: a guest running a million loop iterations never reaches
    // its deadline, and nothing wakes to poll it.
    namespace contract = vtextension::wasm::testing::contract;
    auto const& kind = *WasmtimeTraits::kind();
    auto const backend = WasmtimeTraits::make();
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = runtime->load(assemble(CountingModule));
    REQUIRE(module.has_value());
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(**module, context);

    // Long enough for a ticker, were one moving the epoch, to fire many times over.
    using namespace std::chrono_literals;
    constexpr auto RunFor = 100ms;
    auto const count = contract::functionOf(*instance, "count");
    auto const before = kind.epochCallbacks();
    auto const started = std::chrono::steady_clock::now();
    auto calls = 0;
    auto counted = true;
    while (std::chrono::steady_clock::now() - started < RunFor)
    {
        auto result = std::array<uint32_t, 1> {};
        counted = counted && instance->call(count, result).has_value() && result[0] == CountedTo;
        ++calls;
    }
    INFO("calls of a million iterations each: " << calls);
    CHECK(counted);
    CHECK(kind.epochCallbacks() == before);
}

TEST_CASE("Wasmtime.nestedInterruptStopsTheOuterCall", "[wasmtime]")
{
    // An interrupt landing during a host function call stops the guest that made it, even when the host
    // function calls back in, finds that call interrupted, absorbs the failure and returns into a guest
    // that would spin forever. A regression would hang: the call runs on a thread of its own, and is
    // released by a second interrupt if it does not end in time.
    using namespace std::chrono_literals;
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::CallFailure;
    auto const backend = WasmtimeTraits::make();
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = runtime->load(assemble(R"((module
  (import "contract:test/host@0" "ping" (func $ping (param i32)))
  (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
  (func (export "ping-then-spin") (call $ping (i32.const 0)) (loop (br 0))))
)"));
    REQUIRE(module.has_value());
    auto context = SelfInterruptingHostContext {};
    auto const instance = contract::instantiate(**module, context);
    context.attach(*instance);
    auto const pingThenSpin = contract::functionOf(*instance, "ping-then-spin");

    auto called = std::async(std::launch::async, [&] {
        auto const callGuard = backend->enterThread();
        return instance->call(pingThenSpin, {});
    });
    auto const ended = called.wait_for(5s) == std::future_status::ready;
    if (!ended)
        instance->interrupt();
    auto const result = called.get();
    CHECK(ended);
    CHECK(context.nested() == std::expected<void, CallFailure> { std::unexpected(CallFailure::Interrupted) });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == CallFailure::Interrupted);
}

TEST_CASE("Wasmtime.interruptLeavesSiblingsRunning", "[wasmtime]")
{
    // An interrupt moves the epoch every instance of the runtime shares. One running when it moves wakes
    // once, finds its own instance not interrupted, and runs on; the interrupted one stays stopped.
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::CallFailure;
    auto const& kind = *WasmtimeTraits::kind();
    auto const backend = WasmtimeTraits::make();
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = runtime->load(assemble(CountingModule));
    REQUIRE(module.has_value());
    auto targetContext = contract::RecordingHostContext {};
    auto const target = contract::instantiate(**module, targetContext);
    auto siblingContext = InterruptingHostContext { *target };
    auto const sibling = contract::instantiate(**module, siblingContext);

    auto const before = kind.epochCallbacks();
    auto result = std::array<uint32_t, 1> {};
    REQUIRE(sibling->call(contract::functionOf(*sibling, "ping-then-count"), result).has_value());
    CHECK(result[0] == CountedTo);
    CHECK(kind.epochCallbacks() - before == 1);

    auto const stopped = target->call(contract::functionOf(*target, "count"), result);
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error() == CallFailure::Interrupted);

    // The epoch has not moved since: the sibling's next call runs through without waking.
    REQUIRE(sibling->call(contract::functionOf(*sibling, "count"), result).has_value());
    CHECK(kind.epochCallbacks() - before == 1);
}

TEST_CASE("Wasmtime.interruptRacingACallStopsIt", "[wasmtime]")
{
    // An interrupt landing at any point of a call's way into the guest stops it: before the call checks
    // the flag, between that check and the guest's first instruction, or while the guest runs. Each round
    // releases the interrupt a little later relative to the call than the one before.
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::CallFailure;
    auto const backend = WasmtimeTraits::make();
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = contract::RecordingHostContext {};

    constexpr auto Rounds = 200;
    auto stopped = 0;
    for (auto const round: std::views::iota(0, Rounds))
    {
        auto const instance = contract::instantiate(*module, context);
        auto const spin = contract::functionOf(*instance, "spin");
        auto go = std::atomic<bool> { false };
        auto interrupter = std::jthread([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            // A delay the optimizer cannot drop: an atomic counted up `round * 4` times.
            auto delay = std::atomic<int> { 0 };
            std::ranges::for_each(std::views::iota(0, round * 4),
                                  [&](int) { delay.fetch_add(1, std::memory_order_relaxed); });
            instance->interrupt();
        });
        go.store(true, std::memory_order_release);
        auto const called = instance->call(spin, {});
        interrupter.join();
        if (!called.has_value() && called.error() == CallFailure::Interrupted)
            ++stopped;
    }
    CHECK(stopped == Rounds);
}

TEST_CASE("Wasmtime.measureRender", "[.measure]")
{
    // Reported, not asserted; vtextension_test's twin measures WAMR the same way.
    auto const backend = WasmtimeTraits::make();
    vtextension::wasm::testing::measureBundledRender(*backend, "Wasmtime");
}
