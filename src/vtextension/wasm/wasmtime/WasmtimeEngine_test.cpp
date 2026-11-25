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
#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/EngineFeatures.hpp>
#include <vtextension/wasm/host/MemoryProvision.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/FeatureModules.hpp>
#include <vtextension/wasm/testing/ProcessModules.hpp>
#include <vtextension/wasm/testing/RenderMeasurement.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeEngine.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeMemoryCreator.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeRuntime.hpp>

using vtextension::ExtensionError;
using vtextension::wasm::FeatureState;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::MemoryGrowthReporting;
using vtextension::wasm::ModuleIdentity;
using vtextension::wasm::RuntimeLimits;
using vtextension::wasm::wasmtime::KindState;
using vtextension::wasm::wasmtime::makeKindState;
using vtextension::wasm::wasmtime::makeWasmtimeEngine;
using vtextension::wasm::wasmtime::MemorySharing;
using vtextension::wasm::wasmtime::SignalsGuardBytes;
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
        return makeWasmtimeEngine(kind(), WasmtimeConfig { .trapMode = TrapMode::Software });
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

namespace host = vtextension::wasm::host;
namespace proto = vtextension::wasm::remote::proto;
namespace transport = vtextension::wasm::remote::transport;

/// A guest page.
constexpr auto GuestPage = proto::WasmPageBytes;

/// The object the parent grants each instance of a runtime of the contract's limits.
constexpr auto ContractObjectBytes =
    proto::memoryObjectBytes(vtextension::wasm::testing::contract::Limits.memoryBytes);

/// @return An engine whose instances run in memory they are provided, of the one kind state of this binary.
[[nodiscard]] std::unique_ptr<host::IHostedEngine> makeProvidedEngine(TrapMode trapMode = TrapMode::Software)
{
    return makeWasmtimeEngine(WasmtimeTraits::kind(),
                              WasmtimeConfig { .trapMode = trapMode, .memory = MemorySharing::Provided });
}

/// One instance's memory object as the parent holds it: made, and mapped whole, its pages committed.
class GrantedObject
{
  public:
    /// @param bytes The object's size.
    explicit GrantedObject(uint64_t bytes): _handle { created(bytes) }, _parent { mapped(_handle, bytes) } {}

    /// @return The object's handle, still the parent's.
    [[nodiscard]] transport::NativeHandle handle() const noexcept { return _handle.get(); }

    /// @return The 32-bit little-endian word at @p address, as the parent's mapping reads it.
    [[nodiscard]] uint32_t wordAt(std::size_t address) const
    {
        auto word = uint32_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 4 }))
            word |= std::to_integer<uint32_t>(_parent.bytes()[address + index]) << (index * 8);
        return word;
    }

    /// @return A provision of the object, mapped as the host maps it.
    [[nodiscard]] host::MemoryProvision provision(uint64_t objectBytes,
                                                  uint64_t initialBytes,
                                                  uint64_t guardBytes,
                                                  host::GrowRequest grow) const
    {
        auto mapping =
            transport::GuestMemoryMapping::map(_handle.get(), objectBytes, initialBytes, guardBytes);
        REQUIRE(mapping.has_value());
        return host::MemoryProvision { .memory = std::move(*mapping), .grow = std::move(grow) };
    }

  private:
    [[nodiscard]] static transport::OwnedHandle created(uint64_t bytes)
    {
        auto object = transport::createSharedObject(bytes, transport::SharedObjectUse::GuestMemory);
        REQUIRE(object.has_value());
        return std::move(*object);
    }

    [[nodiscard]] static transport::SharedMapping mapped(transport::OwnedHandle const& handle, uint64_t bytes)
    {
        auto mapping = transport::SharedMapping::map(handle.get(), bytes, transport::MapAccess::ReadWrite);
        REQUIRE(mapping.has_value());
        // Contour commits what it grants through its own view (Windows); the helper does through its own.
        REQUIRE(mapping->commit(0, bytes).has_value());
        return std::move(*mapping);
    }

    transport::OwnedHandle _handle;
    transport::SharedMapping _parent;
};

/// @return A growth request answering every growth with @p status, and recording the size each asked for.
[[nodiscard]] host::GrowRequest answering(proto::GrowStatus status, std::vector<uint64_t>& asked)
{
    return [status, &asked](uint64_t bytes) {
        asked.push_back(bytes);
        return status;
    };
}

/// @return What instantiating @p module with @p provision offered is answered with.
[[nodiscard]] std::expected<std::unique_ptr<vtextension::wasm::IWasmInstance>, ExtensionError>
instantiateWith(vtextension::wasm::IWasmModule& module,
                vtextension::wasm::IHostContext& context,
                host::MemoryProvision provision)
{
    auto const scope = host::ProvisionScope { std::move(provision) };
    return module.instantiate(context);
}

} // namespace

CONTOUR_WASM_BACKEND_CONTRACT(WasmtimeTraits)

TEST_CASE("WasmtimeEngine.kindStampsOutliveNoKind", "[wasmtime]")
{
    // A kind that comes after another, here at the very same address, is not prepared on a thread for having
    // the generation its predecessor prepared it for: a thread's stamp names its kind by serial, not address.
    auto kind = std::optional<KindState> {};
    kind.emplace();
    kind->acquireRuntime();
    auto guard = kind->enterThread();
    REQUIRE(guard.has_value());
    REQUIRE(kind->isCurrentThreadPrepared());
    auto const* const address = &*kind;
    kind->releaseRuntime();
    kind.reset();

    kind.emplace();
    REQUIRE(&*kind == address);
    kind->acquireRuntime();
    CHECK(kind->generation() == (*guard)->generation());
    CHECK_FALSE(kind->isCurrentThreadPrepared());
    auto const renewed = kind->enterThread();
    REQUIRE(renewed.has_value());
    CHECK(kind->isCurrentThreadPrepared());
    guard->reset(); // the predecessor's guard leaves alone the stamp its successor holds
    CHECK(kind->isCurrentThreadPrepared());
    kind->releaseRuntime();
}

TEST_CASE("WasmtimeEngine.hostFunctionArities", "[wasmtime]")
{
    // A host function takes from none up to MaxHostParameters i32s, each reaching the host in order. One
    // taking more cannot be provided, and the runtime is refused.
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::HostFunction;
    using vtextension::wasm::MaxHostParameters;
    auto const backend = WasmtimeTraits::make();

    for (auto const arity: { uint32_t { 0 }, uint32_t { 1 }, MaxHostParameters })
    {
        INFO("arity " << arity);
        auto const hostFunctions =
            std::array { HostFunction { .module = "arity", .name = "f", .parameterCount = arity } };
        auto const runtime = backend->createRuntime(contract::Limits, hostFunctions);
        REQUIRE(runtime.has_value());
        auto const guard = contract::enterThread(*backend);
        auto const module = (*runtime)->load(ModuleIdentity {}, arityModule(arity));
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

TEST_CASE("WasmtimeEngine.hostFunctionCallsBackIn", "[wasmtime]")
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

TEST_CASE("WasmtimeEngine.callsWideExports", "[wasmtime]")
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
    auto const module =
        runtime->load(ModuleIdentity {},
                      assemble(std::format(
                          R"((module (func (export "sum") (param{}) (result i32) {})))", parameters, body)));
    REQUIRE(module.has_value());
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(**module, context);

    // Passed 1, 2, ... 20: 1² + 2² + ... + 20², which no other order of them gives.
    auto argv = std::ranges::to<std::vector>(std::views::iota(uint32_t { 1 }, Width + 1));
    REQUIRE(instance->call(contract::functionOf(*instance, "sum"), argv).has_value());
    CHECK(argv[0] == 2870);
}

TEST_CASE("WasmtimeEngine.refusesWhatWasmtimeEnablesByDefault", "[wasmtime]")
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
        auto const loaded = runtime->load(ModuleIdentity {}, { module.begin(), module.end() });
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

TEST_CASE("WasmtimeEngine.refusesAZeroStack", "[wasmtime]")
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

TEST_CASE("WasmtimeEngine.leavesMemoryGrowthUnreported", "[wasmtime]")
{
    CHECK(WasmtimeTraits::make()->capabilities().memoryGrowth == MemoryGrowthReporting::Unreported);
}

TEST_CASE("WasmtimeEngine.copiesMemoryToTheParent", "[wasmtime]")
{
    // Until a host process can map memory its parent created, the parent reads and writes it by copy.
    auto const engine = makeWasmtimeEngine(WasmtimeTraits::kind(), WasmtimeConfig {});
    CHECK(engine->memoryMode() == vtextension::wasm::remote::proto::MemoryMode::Copy);
}

TEST_CASE("WasmtimeEngine.wasiReadsTheConfiguredEnvironment", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    // A clock reading 7 seconds past its id, and random bytes that are all 0x5A.
    auto config = WasmtimeConfig {
        .trapMode = TrapMode::Software,
        .wasiEnvironment = vtextension::wasm::wasi::ShimEnvironment {
            [](vtextension::wasm::wasi::ClockId clockId) {
                return 7'000'000'000 + uint64_t { static_cast<uint8_t>(clockId) };
            },
            [](std::span<std::byte> bytes) -> std::expected<void, vtextension::wasm::wasi::Errno> {
                std::ranges::fill(bytes, std::byte { 0x5A });
                return {};
            } },
    };
    auto const backend = makeWasmtimeEngine(WasmtimeTraits::kind(), std::move(config));
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
    auto const module = runtime->load(ModuleIdentity {}, assemble(CountingModule));
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
    auto const module = runtime->load(ModuleIdentity {}, assemble(R"((module
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
    auto const module = runtime->load(ModuleIdentity {}, assemble(CountingModule));
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

TEST_CASE("WasmtimeEngine.providedMemoryBacksTheInstance", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    auto const engine = makeProvidedEngine();
    CHECK(engine->memoryMode() == proto::MemoryMode::Shared);
    auto const runtime = contract::createRuntime(*engine);
    auto const guard = contract::enterThread(*engine);
    auto const module = contract::load(*runtime, vtextension::wasm::testing::GrowModule);
    auto const object = GrantedObject { ContractObjectBytes };
    auto asked = std::vector<uint64_t> {};
    auto context = contract::RecordingHostContext {};
    auto instance = instantiateWith(
        *module,
        context,
        object.provision(ContractObjectBytes, GuestPage, 0, answering(proto::GrowStatus::Granted, asked)));
    REQUIRE(instance.has_value());

    auto argv = std::array<uint32_t, 2> { 16, 42 };
    REQUIRE((*instance)->call(contract::functionOf(**instance, "store"), argv).has_value());
    CHECK(object.wordAt(16) == 42);
    CHECK(contract::wordAt(**instance, 16) == 42);
    CHECK(asked.empty());
}

TEST_CASE("WasmtimeEngine.growthAsksAndOpens", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    auto const engine = makeProvidedEngine();
    auto const runtime = contract::createRuntime(*engine);
    auto const guard = contract::enterThread(*engine);
    auto const module = contract::load(*runtime, vtextension::wasm::testing::GrowModule);
    auto const object = GrantedObject { ContractObjectBytes };
    auto context = contract::RecordingHostContext {};
    auto asked = std::vector<uint64_t> {};
    constexpr auto Address = static_cast<uint32_t>(GuestPage + 8);

    SECTION("granted")
    {
        auto instance = instantiateWith(
            *module,
            context,
            object.provision(
                ContractObjectBytes, GuestPage, 0, answering(proto::GrowStatus::Granted, asked)));
        REQUIRE(instance.has_value());
        auto grown = std::array<uint32_t, 1> { 1 };
        REQUIRE((*instance)->call(contract::functionOf(**instance, "grow"), grown).has_value());
        CHECK(grown[0] == 1);
        CHECK(asked == std::vector<uint64_t> { 2 * GuestPage });

        auto argv = std::array<uint32_t, 2> { Address, 7 };
        REQUIRE((*instance)->call(contract::functionOf(**instance, "store"), argv).has_value());
        CHECK(object.wordAt(Address) == 7);
    }

    SECTION("refused")
    {
        auto instance = instantiateWith(
            *module,
            context,
            object.provision(
                ContractObjectBytes, GuestPage, 0, answering(proto::GrowStatus::Refused, asked)));
        REQUIRE(instance.has_value());
        auto grown = std::array<uint32_t, 1> { 1 };
        REQUIRE((*instance)->call(contract::functionOf(**instance, "grow"), grown).has_value());
        CHECK(grown[0] == 0xFFFF'FFFF);
        CHECK(asked == std::vector<uint64_t> { 2 * GuestPage });
        auto const beyond = (*instance)->memory(static_cast<uint32_t>(GuestPage), 1);
        REQUIRE_FALSE(beyond.has_value());
        CHECK(beyond.error() == vtextension::wasm::MemoryFailure::OutOfBounds);

        // The store past the memory traps, rather than reach the closed page.
        auto argv = std::array<uint32_t, 2> { Address, 7 };
        CHECK_FALSE((*instance)->call(contract::functionOf(**instance, "store"), argv).has_value());
    }
}

TEST_CASE("WasmtimeEngine.instantiateWithoutProvisionFails", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    auto const engine = makeProvidedEngine();
    auto const runtime = contract::createRuntime(*engine);
    auto const guard = contract::enterThread(*engine);
    auto const module = contract::load(*runtime, vtextension::wasm::testing::GrowModule);
    auto context = contract::RecordingHostContext {};

    auto const instance = module->instantiate(context);
    REQUIRE_FALSE(instance.has_value());
    CHECK(instance.error() == ExtensionError::InvalidComponent);
}

TEST_CASE("WasmtimeEngine.provisionMismatchFails", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    auto const engine = makeProvidedEngine();
    auto const runtime = contract::createRuntime(*engine);
    auto const guard = contract::enterThread(*engine);
    auto const module = contract::load(*runtime, vtextension::wasm::testing::GrowModule);
    auto context = contract::RecordingHostContext {};
    auto asked = std::vector<uint64_t> {};

    struct Mismatch
    {
        std::string_view what;
        uint64_t objectBytes;
        uint64_t initialBytes;
    };
    for (auto const& [what, objectBytes, initialBytes]: {
             Mismatch { .what = "more initial bytes than the module's minimum",
                        .objectBytes = ContractObjectBytes,
                        .initialBytes = 2 * GuestPage },
             Mismatch { .what = "an object other than the runtime's reservation",
                        .objectBytes = 2 * ContractObjectBytes,
                        .initialBytes = GuestPage },
         })
    {
        INFO(what);
        auto const object = GrantedObject { objectBytes };
        auto const instance = instantiateWith(
            *module,
            context,
            object.provision(objectBytes, initialBytes, 0, answering(proto::GrowStatus::Granted, asked)));
        REQUIRE_FALSE(instance.has_value());
        CHECK(instance.error() == ExtensionError::InvalidComponent);
    }
}

TEST_CASE("WasmtimeEngine.reservationAndGuardAreExplicit", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::wasmtime::testing::lastMemoryRequest;
    using vtextension::wasm::wasmtime::testing::MemoryRequest;

    auto const instantiated = [](TrapMode trapMode) {
        auto const engine = makeProvidedEngine(trapMode);
        auto const runtime = contract::createRuntime(*engine);
        auto const guard = contract::enterThread(*engine);
        auto const module = contract::load(*runtime, vtextension::wasm::testing::GrowModule);
        auto const object = GrantedObject { ContractObjectBytes };
        auto context = contract::RecordingHostContext {};
        auto asked = std::vector<uint64_t> {};
        auto const instance = instantiateWith(*module,
                                              context,
                                              object.provision(ContractObjectBytes,
                                                               GuestPage,
                                                               engine->memoryGuardBytes(),
                                                               answering(proto::GrowStatus::Granted, asked)));
        CHECK(instance.has_value());
        return engine->memoryGuardBytes();
    };

    SECTION("software traps")
    {
        CHECK(instantiated(TrapMode::Software) == 0);
        CHECK(lastMemoryRequest()
              == MemoryRequest { .minimum = GuestPage, .reserved = ContractObjectBytes, .guard = 0 });
    }

    SECTION("signals")
    {
        // A sanitizer or coverage owns the signal handlers: Wasmtime's must not replace them.
        if constexpr (vtextension::wasm::ThisBuildInstrumentation
                      != vtextension::wasm::BuildInstrumentation::None)
            SKIP("an instrumented build traps in software only");
        CHECK(instantiated(TrapMode::Signals) == SignalsGuardBytes);
        CHECK(lastMemoryRequest()
              == MemoryRequest {
                  .minimum = GuestPage, .reserved = ContractObjectBytes, .guard = SignalsGuardBytes });
    }
}

TEST_CASE("WasmtimeEngine.privateMemoryIsUnchanged", "[wasmtime]")
{
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::wasmtime::testing::lastMemoryRequest;
    auto const engine =
        makeWasmtimeEngine(WasmtimeTraits::kind(), WasmtimeConfig { .trapMode = TrapMode::Software });
    CHECK(engine->memoryMode() == proto::MemoryMode::Copy);
    CHECK(engine->memoryGuardBytes() == 0);
    auto const runtime = contract::createRuntime(*engine);
    auto const guard = contract::enterThread(*engine);
    auto const module = contract::load(*runtime, vtextension::wasm::testing::GrowModule);
    auto const object = GrantedObject { ContractObjectBytes };
    auto context = contract::RecordingHostContext {};
    auto asked = std::vector<uint64_t> {};
    auto const requestBefore = lastMemoryRequest();

    // A provision offered to an engine that allocates its own memory is neither asked for nor taken.
    auto const scope = host::ProvisionScope { object.provision(
        ContractObjectBytes, GuestPage, 0, answering(proto::GrowStatus::Refused, asked)) };
    auto const instance = contract::instantiate(*module, context);
    auto grown = std::array<uint32_t, 1> { 1 };
    REQUIRE(instance->call(contract::functionOf(*instance, "grow"), grown).has_value());
    CHECK(grown[0] == 1);
    CHECK(asked.empty());
    CHECK(lastMemoryRequest() == requestBefore);
    CHECK(host::takeProvision().has_value());
}

TEST_CASE("Wasmtime.measureRender", "[.measure]")
{
    // Reported, not asserted; vtextension_test's twin measures WAMR the same way.
    auto const backend = WasmtimeTraits::make();
    vtextension::wasm::testing::measureBundledRender(*backend, "Wasmtime");
}
