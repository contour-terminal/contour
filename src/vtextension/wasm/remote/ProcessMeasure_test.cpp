// SPDX-License-Identifier: Apache-2.0
//
// What a real extension host process costs: a render and a bare call through it, its start, and the memory
// it holds. Hidden `[.measure]` cases, reported and never asserted; run them in a release build.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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
#include <span>
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
#include <vtextension/wasm/remote/HostRefusals.hpp>
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

/// The real host serving as one that copies guest memory where the system could map it (the hostile host's
/// `copy-memory` mode, which serves as the real one otherwise): what sharing memory is measured against.
[[nodiscard]] HelperBackendConfig copyingHelper()
{
    return HelperBackendConfig { .helper = VTEXTENSION_HOSTILE_HELPER_PATH,
                                 .extraArguments = { "--hostile=copy-memory" },
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

/// What Linux states of a process's memory (/proc/<pid>/status and smaps_rollup): what its RLIMIT_DATA counts
/// (VmData), what is resident (VmRSS) and of that what is anonymous (RssAnon) and shared memory (RssShmem,
/// where a shared guest memory's pages count), its proportional share (Pss: a shared page counts once across
/// the processes mapping it), and the most that ever was resident (VmHWM).
struct Footprint
{
    std::size_t dataKiB = 0;                       ///< VmData.
    std::size_t residentKiB = 0;                   ///< VmRSS.
    std::size_t anonymousKiB = 0;                  ///< RssAnon.
    std::size_t sharedMemoryKiB = 0;               ///< RssShmem.
    std::size_t peakKiB = 0;                       ///< VmHWM.
    std::optional<std::size_t> proportionalKiB {}; ///< Pss; only for a process that may be traced.
};

#ifdef __linux__
/// Reads the `name: value kB` lines of a /proc file whose names @p fields lists into the values it points at.
/// @return Whether the file could be opened.
[[nodiscard]] bool readProcFields(std::string const& path,
                                  std::span<std::pair<std::string_view, std::size_t*> const> fields)
{
    auto file = std::ifstream { path };
    if (!file)
        return false;
    for (auto line = std::string {}; std::getline(file, line);)
        for (auto const& [name, value]: fields)
            if (line.starts_with(name))
                *value = std::stoull(line.substr(name.size()));
    return true;
}
#endif

/// @param process What names the process under /proc: its id, or `self`.
/// @return Its footprint; nothing where the system states none. A helper made itself undumpable, so its
///         smaps_rollup, and with it its Pss, is not readable to its parent.
[[nodiscard]] std::optional<Footprint> footprintOf([[maybe_unused]] std::string const& process)
{
#ifdef __linux__
    auto footprint = Footprint {};
    auto const status = std::array {
        std::pair { std::string_view { "VmData:" }, &footprint.dataKiB },
        std::pair { std::string_view { "VmRSS:" }, &footprint.residentKiB },
        std::pair { std::string_view { "RssAnon:" }, &footprint.anonymousKiB },
        std::pair { std::string_view { "RssShmem:" }, &footprint.sharedMemoryKiB },
        std::pair { std::string_view { "VmHWM:" }, &footprint.peakKiB },
    };
    if (!readProcFields(std::format("/proc/{}/status", process), status))
        return std::nullopt;
    auto proportional = std::size_t { 0 };
    auto const rollup = std::array { std::pair { std::string_view { "Pss:" }, &proportional } };
    if (readProcFields(std::format("/proc/{}/smaps_rollup", process), rollup))
        footprint.proportionalKiB = proportional;
    return footprint;
#else
    return std::nullopt;
#endif
}

/// Prints the footprint of @p who, the process /proc names @p process, as it is at @p moment.
void reportFootprint(std::string_view moment, std::string_view who, std::string const& process)
{
    auto const footprint = footprintOf(process);
    if (!footprint)
    {
        std::println(stdout, "{} footprint {}: not stated on this system", who, moment);
        return;
    }
    auto const mebibytes = [](std::size_t kibibytes) {
        return static_cast<double>(kibibytes) / 1024.0;
    };
    auto const proportional = footprint->proportionalKiB
                                  ? std::format("{:.1f} MiB", mebibytes(*footprint->proportionalKiB))
                                  : std::string { "not readable" };
    std::println(stdout,
                 "{} footprint {}: VmData {:.1f} MiB, VmRSS {:.1f} MiB (RssAnon {:.1f}, RssShmem {:.1f}), "
                 "Pss {}, VmHWM {:.1f} MiB",
                 who,
                 moment,
                 mebibytes(footprint->dataKiB),
                 mebibytes(footprint->residentKiB),
                 mebibytes(footprint->anonymousKiB),
                 mebibytes(footprint->sharedMemoryKiB),
                 proportional,
                 mebibytes(footprint->peakKiB));
}

/// Prints the footprint of the helper @p processId as it is at @p moment.
void reportFootprint(std::string_view moment, process::ProcessId processId)
{
    reportFootprint(moment, "Process", std::to_string(processId));
}

/// Times 200 creations of an instance of plasma, the heaviest bundled extension, on a helper @p config names,
/// each released before the next, and prints what was measured: what a pane's extension costs to start, its
/// memory object and grant included where memory is shared.
void measureInstantiate([[maybe_unused]] HelperBackendConfig const& config,
                        [[maybe_unused]] std::string_view name)
{
#ifdef VTEXTENSION_TEST_MODULE_DIR
    constexpr auto MeasuredInstantiations = 200;
    auto const backend = process::testing::binaryComposition().backend(config);
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

    auto services = vtextension::testing::FakeHostServices {};
    auto guestClock = core::ManualClock {};
    auto& clock = core::defaultSteadyClock();
    // One untimed instance first: the helper's first instantiation pays for what later ones reuse.
    REQUIRE(vtextension::Instance::create(**engine, **compiled, services, guestClock, limits).has_value());
    auto durations = std::vector<std::chrono::nanoseconds> {};
    durations.reserve(MeasuredInstantiations);
    for ([[maybe_unused]] auto const index: std::views::iota(0, MeasuredInstantiations))
    {
        auto const start = clock.now();
        auto const instance =
            vtextension::Instance::create(**engine, **compiled, services, guestClock, limits);
        durations.emplace_back(clock.now() - start);
        REQUIRE(instance.has_value());
    }
    auto const times = vtextension::wasm::testing::summarize(std::move(durations));

    using Micros = std::chrono::duration<double, std::micro>;
    std::println(stdout,
                 "{} instantiate plasma ({} instances): median {:.1f} us, p90 {:.1f} us, fastest {:.1f} us",
                 name,
                 MeasuredInstantiations,
                 Micros { times.median }.count(),
                 Micros { times.p90 }.count(),
                 Micros { times.fastest }.count());
#else
    SKIP("the bundled extensions are not built");
#endif
}

} // namespace

TEST_CASE("Process.measureRender", "[.measure]")
{
    // Reported, not asserted; vtextension_remote_test's twin measures a host in this process.
    SECTION("memory shared")
    {
        auto const backend = process::testing::binaryComposition().backend(realHelper());
        vtextension::wasm::testing::measureBundledRender(*backend, "Process");
    }
    SECTION("memory copied")
    {
        auto const backend = process::testing::binaryComposition().backend(copyingHelper());
        vtextension::wasm::testing::measureBundledRender(*backend, "Process (copy)");
    }
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
    auto refusals = vtextension::wasm::remote::HostRefusalLog { vtextension::wasm::remote::errorLogWriter() };

    auto& clock = core::defaultSteadyClock();
    auto durations = std::vector<std::chrono::nanoseconds> {};
    durations.reserve(Incarnations);
    for (auto const incarnation: std::views::iota(0, Incarnations))
    {
        auto const start = clock.now();
        auto const connected = HelperIncarnation::connect(*connector, runtime, policy, refusals);
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

TEST_CASE("Process.measureInstantiate", "[.measure]")
{
    // Reported, not asserted.
    SECTION("memory shared")
    {
        measureInstantiate(realHelper(), "Process");
    }
    SECTION("memory copied")
    {
        measureInstantiate(copyingHelper(), "Process (copy)");
    }
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
    reportFootprint(std::format("{} plasma instances rendered", Instances), "Contour", "self");
#else
    SKIP("the bundled extensions are not built");
#endif
}
