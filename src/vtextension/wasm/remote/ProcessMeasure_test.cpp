// SPDX-License-Identifier: Apache-2.0
//
// What a real extension host process costs: a render and a bare call through it, its start, and the memory
// it holds. Hidden `[.measure]` cases, reported and never asserted; run them in a release build.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/HelperIncarnation.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/testing/RenderMeasurement.hpp>

namespace process = vtextension::wasm::remote::process;
namespace proto = vtextension::wasm::remote::proto;
using process::testing::HelperBackendConfig;
using vtextension::wasm::LockdownRequirement;
using vtextension::wasm::remote::HelperIncarnation;
using vtextension::wasm::remote::HelperPolicy;

namespace
{

/// The real helper, driven although it runs without a sandbox, under the default budgets.
[[nodiscard]] HelperBackendConfig realHelper()
{
    return HelperBackendConfig { .helper = VTEXTENSION_HELPER_PATH,
                                 .extraArguments = {},
                                 .lockdown = LockdownRequirement::AcceptUnsandboxed,
                                 .timeouts = {} };
}

/// @return The process id of the helper the binary's supervisor started last.
[[nodiscard]] process::ProcessId lastHelper()
{
    auto const processIds = process::testing::binaryComposition().recording().processIds();
    REQUIRE_FALSE(processIds.empty());
    return processIds.back();
}

/// What Linux states of a process's memory (/proc/<pid>/status): what its RLIMIT_DATA counts (VmData), what
/// is resident (VmRSS), and the most that ever was (VmHWM).
struct Footprint
{
    std::size_t dataKiB = 0;     ///< VmData.
    std::size_t residentKiB = 0; ///< VmRSS.
    std::size_t peakKiB = 0;     ///< VmHWM.
};

/// @return @p processId's footprint; nothing where the system states none.
[[nodiscard]] std::optional<Footprint> footprintOf([[maybe_unused]] process::ProcessId processId)
{
#ifdef __linux__
    auto status = std::ifstream { std::format("/proc/{}/status", processId) };
    if (!status)
        return std::nullopt;
    auto footprint = Footprint {};
    auto const fields = std::array<std::pair<std::string_view, std::size_t*>, 3> {
        std::pair { std::string_view { "VmData:" }, &footprint.dataKiB },
        std::pair { std::string_view { "VmRSS:" }, &footprint.residentKiB },
        std::pair { std::string_view { "VmHWM:" }, &footprint.peakKiB },
    };
    for (auto line = std::string {}; std::getline(status, line);)
        for (auto const& [name, value]: fields)
            if (line.starts_with(name))
                *value = std::stoull(line.substr(name.size()));
    return footprint;
#else
    return std::nullopt;
#endif
}

/// Prints @p processId's footprint as it is at @p moment.
void reportFootprint(std::string_view moment, process::ProcessId processId)
{
    auto const footprint = footprintOf(processId);
    if (!footprint)
    {
        std::println(stdout, "Process footprint {}: not stated on this system", moment);
        return;
    }
    std::println(stdout,
                 "Process footprint {}: VmData {:.1f} MiB, VmRSS {:.1f} MiB, VmHWM {:.1f} MiB",
                 moment,
                 static_cast<double>(footprint->dataKiB) / 1024.0,
                 static_cast<double>(footprint->residentKiB) / 1024.0,
                 static_cast<double>(footprint->peakKiB) / 1024.0);
}

} // namespace

TEST_CASE("Process.measureRender", "[.measure]")
{
    // Reported, not asserted; vtextension_remote_test's twin measures a host in this process.
    auto const backend = process::testing::binaryComposition().backend(realHelper());
    vtextension::wasm::testing::measureBundledRender(*backend, "Process");
}

TEST_CASE("Process.measureRoundTrip", "[.measure]")
{
    // Reported, not asserted.
    auto const backend = process::testing::binaryComposition().backend(realHelper());
    vtextension::wasm::testing::measureRoundTrip(*backend, "Process");
}

TEST_CASE("Process.measureSpawn", "[.measure]")
{
    // From asking for a host to its runtime created in it: launch, Hello and CreateRuntime. Reported, not
    // asserted; the first incarnation's footprint is what an idle helper holds.
    constexpr auto Incarnations = 20;
    auto const limits = vtextension::ExtensionLimits {};
    auto const runtime = proto::CreateRuntime { .runtimeId = HelperIncarnation::RuntimeId,
                                                .memoryBytes = limits.memoryBytes,
                                                .stackBytes = limits.stackBytes,
                                                .functions = {} };
    auto const config = realHelper();
    auto const connector = process::testing::binaryComposition().connector(config);
    auto const policy = HelperPolicy { .timeouts = config.timeouts, .lockdown = config.lockdown };

    auto& clock = core::defaultSteadyClock();
    auto durations = std::vector<std::chrono::nanoseconds> {};
    durations.reserve(Incarnations);
    for (auto const incarnation: std::views::iota(0, Incarnations))
    {
        auto const start = clock.now();
        auto const connected = HelperIncarnation::connect(*connector, runtime, policy);
        durations.emplace_back(clock.now() - start);
        REQUIRE(connected.has_value());
        if (incarnation == 0)
            reportFootprint("idle", lastHelper());
    }
    auto const times = vtextension::wasm::testing::summarize(std::move(durations));

    using Micros = std::chrono::duration<double, std::micro>;
    std::println(stdout,
                 "Process spawn to runtime created ({} incarnations): median {:.2f} ms, p90 {:.2f} ms, "
                 "fastest {:.2f} ms",
                 Incarnations,
                 Micros { times.median }.count() / 1000.0,
                 Micros { times.p90 }.count() / 1000.0,
                 Micros { times.fastest }.count() / 1000.0);
}

TEST_CASE("Process.measureFootprint", "[.measure]")
{
#ifdef VTEXTENSION_TEST_MODULE_DIR
    // One helper holding the heaviest bundled extension, plasma, loaded once and instantiated many times,
    // each instance rendered once: what the helper's data cap must leave room for. Reported, not asserted.
    constexpr auto Instances = 16;
    auto const backend = process::testing::binaryComposition().backend(realHelper());
    auto limits = vtextension::ExtensionLimits {};
    limits.callDeadline = std::chrono::seconds { 10 };
    limits.loadDeadline = std::chrono::seconds { 60 };
    auto engine = vtextension::Engine::create(*backend, limits);
    REQUIRE(engine.has_value());
    auto const guard = backend->enterThread();
    REQUIRE(guard.has_value());

    auto compiled = vtextension::CompiledExtension::load(
        **engine, {}, vtextension::testing::readTestModule("plasma.wasm"), nullptr);
    REQUIRE(compiled.has_value());
    auto const helper = lastHelper();
    reportFootprint("plasma loaded", helper);

    auto services = vtextension::testing::FakeHostServices {};
    services.snapshot.focus = vtextension::FocusState::Focused;
    auto guestClock = core::ManualClock {};
    auto instances = std::vector<std::unique_ptr<vtextension::Instance>> {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, Instances))
    {
        auto instance = vtextension::Instance::create(**engine, **compiled, services, guestClock, limits);
        REQUIRE(instance.has_value());
        REQUIRE((*instance)->load({}).has_value());
        REQUIRE((*instance)->render(vtextension::testing::surface(120, 1)).has_value());
        instances.push_back(std::move(*instance));
    }
    CHECK(lastHelper() == helper);
    reportFootprint(std::format("{} plasma instances rendered", Instances), helper);
#else
    SKIP("the bundled extensions are not built");
#endif
}
