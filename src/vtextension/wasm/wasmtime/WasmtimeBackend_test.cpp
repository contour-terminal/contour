// SPDX-License-Identifier: Apache-2.0
//
// The Wasmtime backend: the backend contract every runtime must meet, and what is Wasmtime's alone,
// the features it enables unless told otherwise, checked with its C API directly.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

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

} // namespace

CONTOUR_WASM_BACKEND_CONTRACT(WasmtimeTraits)

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
