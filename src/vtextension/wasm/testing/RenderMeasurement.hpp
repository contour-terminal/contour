// SPDX-License-Identifier: Apache-2.0
//
// Test-only: times an extension's `render` on a given backend, so that two backends, which cannot share a
// test binary, are measured by the same workload, iteration count and timing method. Each backend's suite
// runs it from a hidden `[.measure]` case; the numbers are reported, never asserted.
#pragma once

#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm::testing
{

/// How many times measureRender() calls `render`.
inline constexpr auto MeasuredRenders = std::size_t { 1000 };

/// What measureRender() measured.
struct RenderMeasurement
{
    std::chrono::nanoseconds load {};    ///< Loading (compiling) the module, once.
    std::chrono::nanoseconds start {};   ///< Instantiating it and calling its `load`, once.
    std::chrono::nanoseconds median {};  ///< The median of the timed `render` calls.
    std::chrono::nanoseconds p90 {};     ///< Their 90th percentile.
    std::chrono::nanoseconds fastest {}; ///< The fastest of them.
    std::size_t outputBytes = 0;         ///< What the last `render` returned, in bytes.
};

/// Loads @p module on @p backend, instantiates it and times @p renders calls of its `render`, at each of
/// @p sizes in turn, after one untimed warm-up call. Compilation is timed apart from the calls.
/// @param backend The backend under measurement.
/// @param module The extension's module.
/// @param sizes The surfaces the calls draw, cycled through; at least one.
/// @param renders How many `render` calls to time; at least one.
/// @param clock What the durations are read from.
/// @return What was measured.
[[nodiscard]] inline RenderMeasurement measureRender(IWasmBackend& backend,
                                                     std::vector<std::byte> module,
                                                     std::span<vtpty::PageSize const> sizes,
                                                     std::size_t renders,
                                                     core::IClock& clock)
{
    REQUIRE(renders > 0);
    REQUIRE_FALSE(sizes.empty());
    // Generous deadlines: the watchdog must not cut a slow backend's calls short, or its median would
    // describe a failure rather than the work.
    auto limits = ExtensionLimits {};
    limits.callDeadline = std::chrono::seconds { 10 };
    limits.loadDeadline = std::chrono::seconds { 60 };

    auto engine = Engine::create(backend, limits);
    REQUIRE(engine.has_value());
    // Calls in on this thread, which a backend may not have prepared by creating the engine alone.
    auto const guard = backend.enterThread();
    REQUIRE(guard.has_value());
    auto services = vtextension::testing::FakeHostServices {};
    services.snapshot.focus = FocusState::Focused;
    auto guestClock = core::ManualClock {};

    auto measurement = RenderMeasurement {};
    auto const loadStart = clock.now();
    auto compiled = CompiledExtension::load(**engine, ModuleIdentity {}, std::move(module), nullptr);
    measurement.load = clock.now() - loadStart;
    REQUIRE(compiled.has_value());

    auto const startStart = clock.now();
    auto instance = Instance::create(**engine, **compiled, services, guestClock, limits);
    REQUIRE(instance.has_value());
    REQUIRE((*instance)->load({}).has_value());
    measurement.start = clock.now() - startStart;

    REQUIRE((*instance)->render(sizes.front()).has_value());
    auto durations = std::vector<std::chrono::nanoseconds> {};
    durations.reserve(renders);
    for (auto const call: std::views::iota(std::size_t { 0 }, renders))
    {
        auto const size = sizes[(call + 1) % sizes.size()];
        auto const callStart = clock.now();
        auto const rendered = (*instance)->render(size);
        durations.emplace_back(clock.now() - callStart);
        REQUIRE(rendered.has_value());
        measurement.outputBytes = rendered->size();
    }
    std::ranges::sort(durations);
    measurement.median = durations[durations.size() / 2];
    measurement.p90 = durations[std::min(durations.size() - 1, durations.size() * 9 / 10)];
    measurement.fastest = durations.front();
    return measurement;
}

/// Prints @p measurement of @p backendName on one line, for the person who ran the hidden case.
inline void reportRender(std::string_view backendName,
                         std::string_view workload,
                         RenderMeasurement const& measurement)
{
    using Micros = std::chrono::duration<double, std::micro>;
    std::println(
        stdout,
        "{} {}: render median {:.1f} us, p90 {:.1f} us, fastest {:.1f} us ({} bytes); load {:.1f} ms, "
        "start {:.1f} ms",
        backendName,
        workload,
        Micros { measurement.median }.count(),
        Micros { measurement.p90 }.count(),
        Micros { measurement.fastest }.count(),
        measurement.outputBytes,
        Micros { measurement.load }.count() / 1000.0,
        Micros { measurement.start }.count() / 1000.0);
}

/// Measures the heaviest bundled extension, plasma, on @p backend and prints what was measured: its
/// steady frame (`render(120, 1)`, the pixels encoded once and repeated, the palette computed anew), and
/// its uncached frame (alternating 120 and 119 columns, so that every call encodes the pixels again).
/// Skips the calling case where the bundled extensions are not built.
/// @param backend The backend under measurement.
/// @param backendName What names @p backend in the report.
inline void measureBundledRender([[maybe_unused]] IWasmBackend& backend,
                                 [[maybe_unused]] std::string_view backendName)
{
#ifdef VTEXTENSION_TEST_MODULE_DIR
    using vtextension::testing::surface;
    auto const steady = std::array { surface(120, 1) };
    auto const uncached = std::array { surface(120, 1), surface(119, 1) };
    auto const module = vtextension::testing::readTestModule("plasma.wasm");
    reportRender(backendName,
                 "steady",
                 measureRender(backend, module, steady, MeasuredRenders, core::defaultSteadyClock()));
    reportRender(backendName,
                 "uncached",
                 measureRender(backend, module, uncached, MeasuredRenders, core::defaultSteadyClock()));
#else
    SKIP("the bundled extensions are not built");
#endif
}

} // namespace vtextension::wasm::testing
