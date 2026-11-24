// SPDX-License-Identifier: Apache-2.0
//
// The Wasmtime backend: the backend contract every runtime must meet, and what is Wasmtime's alone,
// the features it enables unless told otherwise, checked with its C API directly.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <wasmtime.h>

#ifndef _WIN32
    #include <csignal>
#endif

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/EngineFeatures.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/FeatureModules.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeBackend.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>

using vtextension::ExtensionError;
using vtextension::wasm::FeatureState;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::MemoryGrowthReporting;
using vtextension::wasm::RuntimeLimits;
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
        static auto const kind = makeKindState();
        return makeWasmtimeBackend(kind, WasmtimeConfig { .trapMode = TrapMode::Software });
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
                        .stackBytes = 0 },
        vtextension::wasm::testing::contract::HostFunctions);
    REQUIRE_FALSE(runtime.has_value());
    CHECK(runtime.error() == ExtensionError::RuntimeUnavailable);
}

TEST_CASE("WasmtimeBackend.leavesMemoryGrowthUnreported", "[wasmtime]")
{
    CHECK(WasmtimeTraits::make()->capabilities().memoryGrowth == MemoryGrowthReporting::Unreported);
}
