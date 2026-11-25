// SPDX-License-Identifier: Apache-2.0
//
// The extension host executable itself, started as Contour starts it.
#include <core/Clock.hpp>
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/process/testing/RecordingLauncher.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/ProcessModules.hpp>

namespace proto = vtextension::wasm::remote::proto;
namespace process = vtextension::wasm::remote::process;
using process::ExitKind;
using process::ExitStatus;
using process::testing::awaitReap;
using process::testing::HelperLaunch;
using process::testing::launchHelper;
using process::testing::ownProcessId;
using vtextension::ExtensionError;
using vtextension::testing::toModule;
using vtextension::wasm::BackendConfig;
using vtextension::wasm::HostTimeouts;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::LockdownRequirement;
using vtextension::wasm::ModuleIdentity;
using vtextension::wasm::remote::transport::PduStream;
using namespace std::chrono_literals;
namespace contract = vtextension::wasm::testing::contract;

namespace
{

/// The sanitizer variables a helper is handed when this process has them, so that it reports as the tests do.
constexpr auto SanitizerVariables =
    std::array<std::string_view, 4> { "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS" };

/// @return The helper's environment: the launcher's default, and this process's sanitizer variables.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> helperEnvironment()
{
    auto environment = process::defaultHelperEnvironment();
    auto const live = core::LiveEnvironment {};
    for (auto const name: SanitizerVariables)
        if (auto value = live.get(name))
            environment.emplace_back(std::string { name }, *std::move(value));
    return environment;
}

/// @return A launch of the extension host executable with @p leading arguments before its own.
[[nodiscard]] HelperLaunch helperLaunch(std::vector<std::string> leading = {},
                                        process::ProcessId parentProcessId = ownProcessId())
{
    return HelperLaunch { .executable = VTEXTENSION_HELPER_PATH,
                          .leading = std::move(leading),
                          .trailing = {},
                          .parentProcessId = parentProcessId,
                          .dataBytes = process::DefaultDataBytes,
                          .environment = helperEnvironment() };
}

#ifdef __linux__
/// A limit's soft and hard value, as /proc/<pid>/limits states them.
struct LimitValues
{
    std::string soft;
    std::string hard;

    bool operator==(LimitValues const&) const = default;
};

/// @return The soft and hard value of limit @p name (as `Max processes`) of process @p processId; nothing if
///         it is not listed.
[[nodiscard]] std::optional<LimitValues> limitOf(process::ProcessId processId, std::string_view name)
{
    auto limits = std::ifstream { std::format("/proc/{}/limits", processId) };
    auto line = std::string {};
    while (std::getline(limits, line))
    {
        if (!line.starts_with(name))
            continue;
        auto values = std::istringstream { line.substr(name.size()) };
        auto result = LimitValues {};
        values >> result.soft >> result.hard;
        return result;
    }
    return std::nullopt;
}
#endif

/// Receives the Hello on the parent end of @p helper's control channel, which must arrive.
/// @return The control stream, still open.
[[nodiscard]] PduStream receiveHello(process::testing::LaunchedHelper& helper)
{
    auto control = PduStream { std::move(helper.control),
                               proto::ChannelKind::Control,
                               proto::Direction::ToParent,
                               proto::PduLimits {} };
    auto const hello = control.receive();
    REQUIRE(hello.has_value());
    CHECK(*hello
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = proto::LockdownState::Unsandboxed } });
    return control;
}

/// How long a teardown may take: the helpers exit on their own once their channels close (D57's grace).
constexpr auto TeardownGrace = std::chrono::milliseconds { 2000 };

/// The binary's supervisor, as a hosting process has one: it starts the real helper, recorded.
struct ProcessRig
{
    std::shared_ptr<process::testing::RecordingLauncher> recording =
        std::make_shared<process::testing::RecordingLauncher>(process::makeSystemProcessLauncher());
    std::unique_ptr<process::ExtensionHostSupervisor> supervisor = [this] {
        auto started = process::ExtensionHostSupervisor::start(process::SupervisorConfig {
            .launcher = recording, .clock = &core::defaultSteadyClock(), .teardownGrace = TeardownGrace });
        REQUIRE(started.has_value());
        return std::move(*started);
    }();
    std::shared_ptr<vtextension::wasm::remote::RemoteKindState> kind =
        vtextension::wasm::remote::makeRemoteKindState();
};

/// @return The binary-wide rig.
[[nodiscard]] ProcessRig& processRig()
{
    static auto rig = ProcessRig {};
    return rig;
}

/// @return A remote backend whose hosts are real helper processes, under @p timeouts, driving those
///         confined as @p lockdown asks.
[[nodiscard]] std::unique_ptr<IWasmBackend> processBackend(LockdownRequirement lockdown,
                                                           HostTimeouts timeouts = {})
{
    auto& rig = processRig();
    auto connector = std::make_shared<process::ProcessHostConnector>(
        rig.supervisor->mailbox(),
        process::ProcessHostConfig { .helper = VTEXTENSION_HELPER_PATH,
                                     .environment = helperEnvironment(),
                                     .extraArguments = {},
                                     .caps = {} });
    return vtextension::wasm::remote::makeRemoteBackend(rig.kind,
                                                        BackendConfig { .helper = VTEXTENSION_HELPER_PATH,
                                                                        .timeouts = timeouts,
                                                                        .connector = connector,
                                                                        .lockdown = lockdown });
}

/// @return How many kills the binary's supervisor sent for @p reason so far, as of everything it was told
///         and everything that fell due by now: a kill's onDeath runs before its batch's counters publish.
[[nodiscard]] std::size_t killsFor(process::KillReason reason)
{
    auto& supervisor = *processRig().supervisor;
    supervisor.sync();
    return supervisor.counters().kills.at(std::to_underlying(reason));
}

/// Waits until every helper of the binary's supervisor was reaped -- those let go exit on their own -- and
/// checks that none ended uncleanly: how a helper's sanitizer report fails a case.
void checkHelpersEndedCleanly()
{
    auto const& supervisor = *processRig().supervisor;
    auto const deadline = std::chrono::steady_clock::now() + TeardownGrace + 3s;
    while (supervisor.liveProcesses() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    CHECK(supervisor.liveProcesses() == 0);
    CHECK(supervisor.counters().uncleanExits == 0);
}

} // namespace

TEST_CASE("Helper.saysHelloOverInheritedChannels", "[vtextension]")
{
    auto helper = launchHelper(process::makeSystemProcessLauncher(), helperLaunch());
    REQUIRE(helper.has_value());
    auto control = receiveHello(*helper);

    // Contour closes the connection: the helper exits cleanly, its sanitizers' reports included.
    {
        auto const closed = std::move(control);
    }
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 0 });
}

TEST_CASE("Helper.exitsOnBadArguments", "[vtextension]")
{
    auto helper = launchHelper(process::makeSystemProcessLauncher(), helperLaunch({ "--bogus=1" }));
    REQUIRE(helper.has_value());
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 70 });
}

#ifndef _WIN32
TEST_CASE("Helper.exitsWhenItsParentIsAnother", "[vtextension]")
{
    auto helper = launchHelper(process::makeSystemProcessLauncher(), helperLaunch({}, 1));
    REQUIRE(helper.has_value());
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 72 });
}
#endif

#ifdef __linux__
TEST_CASE("Helper.capsItselfBeforeHello", "[vtextension]")
{
    if constexpr (vtextension::wasm::host::BuildCapPolicy == vtextension::wasm::host::CapPolicy::Skip)
        SKIP("sanitizer builds apply no caps");
    constexpr auto DataBytes = uint64_t { 768 } * 1024 * 1024;
    auto launch = helperLaunch();
    launch.dataBytes = DataBytes;
    auto helper = launchHelper(process::makeSystemProcessLauncher(), std::move(launch));
    REQUIRE(helper.has_value());
    auto control = receiveHello(*helper);

    // Capped before Hello, soft and hard alike: no further thread, and the data bytes it was told of.
    auto const processId = helper->process->id();
    CHECK(limitOf(processId, "Max processes") == LimitValues { .soft = "0", .hard = "0" });
    CHECK(limitOf(processId, "Max data size")
          == LimitValues { .soft = std::to_string(DataBytes), .hard = std::to_string(DataBytes) });

    {
        auto const closed = std::move(control);
    }
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 0 });
}
#endif

TEST_CASE("Process.loadsAndCallsThroughARealHelper", "[vtextension]")
{
    {
        auto const backend = processBackend(LockdownRequirement::AcceptUnsandboxed);
        auto const runtime = contract::createRuntime(*backend);
        auto const guard = contract::enterThread(*backend);
        auto const module = contract::load(*runtime, contract::InstanceModule);
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(*module, context);
        auto argv = std::array<uint32_t, 2> { 2, 3 };
        REQUIRE(instance->call(contract::functionOf(*instance, "add"), argv).has_value());
        CHECK(argv[0] == 5);
    }
    checkHelpersEndedCleanly();
}

TEST_CASE("Process.unsandboxedIsRefusedUnlessAccepted", "[vtextension]")
{
    auto const requested = killsFor(process::KillReason::Requested);
    {
        // The helper runs without a sandbox until it has one, and says so in its Hello.
        auto const backend = processBackend(LockdownRequirement::RequireLockedDown);
        auto const runtime = contract::createRuntime(*backend);
        auto const loaded = runtime->load(ModuleIdentity {}, toModule(contract::InstanceModule));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ExtensionError::HostUnavailable);
    }
    checkHelpersEndedCleanly();
    CHECK(killsFor(process::KillReason::Requested) == requested + 1);
}

TEST_CASE("Process.startFunctionThatNeverReturnsIsKilledAtTheInstantiateTimeout", "[vtextension]")
{
    auto const timeouts = killsFor(process::KillReason::StepTimeout);
    auto const launches = processRig().recording->launches();
    {
        auto const backend =
            processBackend(LockdownRequirement::AcceptUnsandboxed, HostTimeouts { .instantiate = 200ms });
        auto const runtime = contract::createRuntime(*backend);
        auto const guard = contract::enterThread(*backend);
        auto const identity = ModuleIdentity { .key = "spin" };
        auto const spinning = runtime->load(identity, toModule(vtextension::wasm::testing::StartSpinModule));
        REQUIRE(spinning.has_value());
        auto context = contract::RecordingHostContext {};
        auto const start = std::chrono::steady_clock::now();
        auto const instantiated = (*spinning)->instantiate(context);
        auto const took = std::chrono::steady_clock::now() - start;
        REQUIRE_FALSE(instantiated.has_value());
        CHECK(instantiated.error() == ExtensionError::HostUnavailable);
        CHECK(took >= 200ms);
        CHECK(took < 5s);
        CHECK(killsFor(process::KillReason::StepTimeout) == timeouts + 1);

        // The same identity's next module runs in a helper started anew.
        auto const module = runtime->load(identity, toModule(contract::InstanceModule));
        REQUIRE(module.has_value());
        auto const instance = contract::instantiate(**module, context);
        auto argv = std::array<uint32_t, 2> { 20, 22 };
        REQUIRE(instance->call(contract::functionOf(*instance, "add"), argv).has_value());
        CHECK(argv[0] == 42);
        CHECK(processRig().recording->launches() == launches + 2);
    }
    checkHelpersEndedCleanly();
}
