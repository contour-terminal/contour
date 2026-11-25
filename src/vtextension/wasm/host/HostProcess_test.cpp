// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/IHostedEngine.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>
#include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/HostedEngine.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>

    #include <unistd.h>
#endif

using vtextension::testing::toModule;
using vtextension::wasm::CallFailure;
using vtextension::wasm::host::BootstrapError;
using vtextension::wasm::host::CapPolicy;
using vtextension::wasm::host::capResources;
using vtextension::wasm::host::EngineFactory;
using vtextension::wasm::host::exitCodeOf;
using vtextension::wasm::host::HardeningRow;
using vtextension::wasm::host::HardeningStep;
using vtextension::wasm::host::hardeningSteps;
using vtextension::wasm::host::hardeningStepText;
using vtextension::wasm::host::hardenProcess;
using vtextension::wasm::host::HostConfinement;
using vtextension::wasm::host::HostProcessExit;
using vtextension::wasm::host::hostProcessExitOf;
using vtextension::wasm::host::hostProcessExits;
using vtextension::wasm::host::IHostedEngine;
using vtextension::wasm::host::runExtensionHost;
using vtextension::wasm::host::ServerExit;
using vtextension::wasm::remote::MaxLaneChannels;
using vtextension::wasm::remote::transport::Channel;
using vtextension::wasm::remote::transport::ControlPage;
using vtextension::wasm::remote::transport::formatHelperArguments;
using vtextension::wasm::remote::transport::HelperArguments;
using vtextension::wasm::remote::transport::makeChannelPair;
using vtextension::wasm::remote::transport::makeWakePair;
using vtextension::wasm::remote::transport::NativeHandle;
using vtextension::wasm::remote::transport::PduStream;
using vtextension::wasm::remote::transport::StreamError;
using vtextension::wasm::remote::transport::WakeSender;
using vtextension::wasm::remote::transport::testing::closeHandle;
using vtextension::wasm::remote::transport::testing::duplicate;
using vtextension::wasm::testing::makeHostedEngine;

namespace proto = vtextension::wasm::remote::proto;
namespace contract = vtextension::wasm::testing::contract;

namespace
{

/// How long a test waits for the host to end.
constexpr auto HostPatience = std::chrono::seconds { 5 };

/// The parent process id the tests' confinement accepts.
constexpr auto TheParent = int64_t { 4242 };

/// The data cap the tests hand the host.
constexpr auto TheDataBytes = uint64_t { 64 } * 1024 * 1024;

/// @return This process's id.
[[nodiscard]] int64_t ownProcessId() noexcept
{
#ifdef _WIN32
    return static_cast<int64_t>(::GetCurrentProcessId());
#else
    return static_cast<int64_t>(::getpid());
#endif
}

/// @return How many threads this process runs; 0 where that is not known (not Linux).
[[nodiscard]] std::size_t threadCount()
{
#ifdef __linux__
    auto const tasks = std::filesystem::directory_iterator { "/proc/self/task" };
    return static_cast<std::size_t>(std::ranges::distance(tasks, std::filesystem::directory_iterator {}));
#else
    return 0;
#endif
}

/// What the tests' confinement saw. Each step records when it ran, as the count of steps before it.
struct Confined
{
    std::atomic<int64_t> boundTo { 0 };
    std::atomic<uint64_t> cappedTo { 0 };
    std::atomic<int> confines { 0 };
    std::atomic<int> hardenings { 0 };
    std::atomic<int> steps { 0 };
    std::atomic<int> hardenedAt { -1 };
    std::atomic<int> engineMadeAt { -1 };
    std::atomic<int> confinedAt { -1 };
    std::atomic<std::size_t> threadsWhenHardened { 0 };
    std::atomic<std::size_t> threadsWhenConfined { 0 };
    /// What beforeThreads returns.
    std::expected<void, HardeningStep> hardening {};
};

/// @return A confinement that touches nothing of this process: it binds to TheParent alone, and hardens and
///         caps by recording that, and when, it did.
[[nodiscard]] HostConfinement testConfinement(std::shared_ptr<Confined> const& confined)
{
    return HostConfinement {
        .bindToParent = [confined](int64_t parent) -> std::expected<void, BootstrapError> {
            confined->boundTo.store(parent);
            if (parent != TheParent)
                return std::unexpected(BootstrapError::ParentGone);
            return {};
        },
        .beforeThreads =
            [confined] {
                confined->threadsWhenHardened.store(threadCount());
                confined->hardenedAt.store(confined->steps.fetch_add(1));
                confined->hardenings.fetch_add(1);
                return confined->hardening;
            },
        .confine =
            [confined](uint64_t dataBytes) {
                confined->threadsWhenConfined.store(threadCount());
                confined->confinedAt.store(confined->steps.fetch_add(1));
                confined->cappedTo.store(dataBytes);
                confined->confines.fetch_add(1);
                return capResources(dataBytes, CapPolicy::Skip);
            },
    };
}

/// @return An engine factory that records when it made the engine, among @p confined's steps.
[[nodiscard]] EngineFactory recordingEngines(std::shared_ptr<Confined> confined)
{
    return [confined = std::move(confined)]() -> std::unique_ptr<IHostedEngine> {
        confined->engineMadeAt.store(confined->steps.fetch_add(1));
        return makeHostedEngine();
    };
}

/// Contour's ends of everything a host is handed, and the command line that hands the host its own.
struct ParentEnds
{
    PduStream control;
    std::vector<Channel> lanes;
    ControlPage page;
    WakeSender wake;
    HelperArguments arguments;
};

/// @return Fresh channels, page and wake, the helper's ends as second handles in the arguments.
[[nodiscard]] ParentEnds makeParentEnds()
{
    auto const handOver = [](NativeHandle handle) {
        auto const copy = duplicate(handle);
        REQUIRE(copy.has_value());
        return *copy;
    };
    auto control = makeChannelPair();
    REQUIRE(control.has_value());
    auto page = ControlPage::create();
    REQUIRE(page.has_value());
    auto wake = makeWakePair();
    REQUIRE(wake.has_value());
    auto arguments = HelperArguments { .control = handOver(control->helper.nativeHandle()),
                                       .lanes = {},
                                       .controlPage = handOver(page->nativeHandle()),
                                       .wake = handOver(wake->helper.nativeHandle()),
                                       .parentProcessId = TheParent,
                                       .dataBytes = TheDataBytes };
    auto lanes = std::vector<Channel> {};
    for (auto& lane: arguments.lanes)
    {
        auto pair = makeChannelPair();
        REQUIRE(pair.has_value());
        lane = handOver(pair->helper.nativeHandle());
        lanes.push_back(std::move(pair->parent));
    }
    return ParentEnds { .control = PduStream { std::move(control->parent),
                                               proto::ChannelKind::Control,
                                               proto::Direction::ToParent,
                                               proto::PduLimits {} },
                        .lanes = std::move(lanes),
                        .page = *std::move(page),
                        .wake = std::move(wake->parent),
                        .arguments = arguments };
}

/// Closes every handle @p arguments name: what a host that ended before adopting them leaves.
void closeAll(HelperArguments const& arguments) noexcept
{
    closeHandle(arguments.control);
    for (auto const lane: arguments.lanes)
        closeHandle(lane);
    closeHandle(arguments.controlPage);
    closeHandle(arguments.wake);
}

/// Runs the host with @p arguments to its end.
/// @return Its exit code.
[[nodiscard]] int runHost(std::vector<std::string> const& arguments,
                          EngineFactory const& engines,
                          HostConfinement const& confinement)
{
    auto views = std::vector<std::string_view> {};
    for (auto const& argument: arguments)
        views.emplace_back(argument);
    return runExtensionHost(views, engines, confinement);
}

/// An engine factory that counts its calls.
[[nodiscard]] EngineFactory countingEngines(std::shared_ptr<std::atomic<int>> calls)
{
    return [calls = std::move(calls)]() -> std::unique_ptr<IHostedEngine> {
        calls->fetch_add(1);
        return makeHostedEngine();
    };
}

/// A host serving over a parent's ends on a thread of its own, the contract's InstanceModule instantiated in
/// it as instance 1 with interrupt slot InstanceSlot: what the interrupt cases drive.
struct ServingHost
{
    static constexpr auto InstanceSlot = uint32_t { 17 };

    ParentEnds parent = makeParentEnds();
    std::shared_ptr<Confined> confined = std::make_shared<Confined>();
    std::vector<std::string> arguments = formatHelperArguments(parent.arguments);
    std::packaged_task<int()> task { [this] {
        return runHost(arguments, [] { return makeHostedEngine(); }, testConfinement(confined));
    } };
    std::future<int> exitCode = task.get_future();
    std::jthread host { std::move(task) };
    /// Ends the connection as it goes, before the host's thread is joined: however the case ends, a failed
    /// REQUIRE included, the host stops serving rather than keep the join waiting.
    struct ConnectionEnd
    {
        explicit ConnectionEnd(PduStream& stream) noexcept: control { stream } {}
        ConnectionEnd(ConnectionEnd const&) = delete;
        ConnectionEnd(ConnectionEnd&&) = delete;
        ConnectionEnd& operator=(ConnectionEnd const&) = delete;
        ConnectionEnd& operator=(ConnectionEnd&&) = delete;
        ~ConnectionEnd() { control.shutdown(); }

        PduStream& control;
    } end { parent.control };
    PduStream lane { std::move(parent.lanes.front()),
                     proto::ChannelKind::Lane,
                     proto::Direction::ToParent,
                     proto::PduLimits {} };
    proto::Loaded loaded = [this] {
        REQUIRE(std::holds_alternative<proto::Hello>(receive()));
        auto functions = std::vector<proto::WireHostFunction> {};
        for (auto const& function: contract::HostFunctions)
            functions.push_back(proto::WireHostFunction { .module = std::string { function.module },
                                                          .name = std::string { function.name },
                                                          .parameterCount = function.parameterCount });
        send(proto::CreateRuntime { .runtimeId = 1,
                                    .memoryBytes = contract::Limits.memoryBytes,
                                    .stackBytes = contract::Limits.stackBytes,
                                    .tableElements = contract::Limits.tableElements,
                                    .functions = std::move(functions) });
        REQUIRE(std::holds_alternative<proto::RuntimeCreated>(receive()));
        send(proto::Load { .runtimeId = 1, .moduleId = 1, .bytes = toModule(contract::InstanceModule) });
        auto answer = receive();
        REQUIRE(std::holds_alternative<proto::Loaded>(answer));
        send(proto::Instantiate { .moduleId = 1, .instanceId = 1, .interruptSlot = InstanceSlot });
        auto const instantiated = receive();
        REQUIRE(std::holds_alternative<proto::Instantiated>(instantiated));
        REQUIRE_FALSE(std::get<proto::Instantiated>(instantiated).error.has_value());
        return std::get<proto::Loaded>(std::move(answer));
    }();

    ServingHost() = default;
    ServingHost(ServingHost const&) = delete;
    ServingHost(ServingHost&&) = delete;
    ServingHost& operator=(ServingHost const&) = delete;
    ServingHost& operator=(ServingHost&&) = delete;

    /// Ends the connection, and checks the host ends as having served.
    ~ServingHost()
    {
        parent.control.shutdown();
        if (exitCode.wait_for(HostPatience) == std::future_status::ready)
            CHECK(exitCode.get() == exitCodeOf(HostProcessExit::Served));
        else
        {
            // Joining its thread would wait forever: the run ends here, rather than hang.
            FAIL_CHECK("the host did not end with its connection");
            std::fputs("HostProcess: the host did not end with its connection\n", stderr);
            std::abort();
        }
    }

    void send(proto::Pdu const& pdu) { REQUIRE(parent.control.send(pdu).has_value()); }

    [[nodiscard]] proto::Pdu receive()
    {
        auto pdu = parent.control.receive();
        REQUIRE(pdu.has_value());
        return std::move(*pdu);
    }

    /// Calls the instance's export @p name with @p argv on lane 0.
    void call(std::string_view name, std::vector<uint32_t> argv)
    {
        auto const named =
            std::ranges::find(loaded.exports, name, &vtextension::wasm::ExportDescription::name);
        REQUIRE(named != loaded.exports.end());
        auto const ordinal = static_cast<uint32_t>(named - loaded.exports.begin());
        // Sent outside the assertion, which may spell its expression twice.
        auto const sent =
            lane.send(proto::Call { .instanceId = 1, .exportOrdinal = ordinal, .argv = std::move(argv) });
        REQUIRE(sent.has_value());
    }

    /// @return The next PDU on lane 0, which must arrive within HostPatience: a case waiting on an answer
    /// that
    ///         never comes ends the connection instead, which ends the wait, and fails.
    [[nodiscard]] proto::Pdu receiveOnLane()
    {
        auto received = std::async(std::launch::async, [this] { return lane.receive(); });
        if (received.wait_for(HostPatience) != std::future_status::ready)
        {
            FAIL_CHECK("no answer on the lane within the deadline");
            parent.control.shutdown();
        }
        auto pdu = received.get();
        REQUIRE(pdu.has_value());
        return std::move(*pdu);
    }
};

} // namespace

TEST_CASE("HostProcess.raisedFlagAndWakeInterrupt", "[vtextension][host]")
{
    auto serving = ServingHost {};
    serving.call("spin", {});
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });

    // What Contour does to interrupt: raise the slot, then wake the host to look.
    auto const raisedAt = std::chrono::steady_clock::now();
    serving.parent.page.raise(ServingHost::InstanceSlot);
    std::ignore = serving.parent.wake.signal();
    CHECK(serving.receiveOnLane()
          == proto::Pdu { proto::CallResult { .failure = CallFailure::Interrupted, .argv = {} } });
    CHECK(std::chrono::steady_clock::now() - raisedAt < std::chrono::seconds { 1 });
}

TEST_CASE("HostProcess.wakeWithoutAFlagInterruptsNothing", "[vtextension][host]")
{
    auto serving = ServingHost {};
    std::ignore = serving.parent.wake.signal();
    serving.parent.page.raise(ServingHost::InstanceSlot + 1); // no instance holds it
    std::ignore = serving.parent.wake.signal();
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    serving.call("add", { 2, 3 });
    CHECK(serving.receiveOnLane()
          == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 5 } } });
}

TEST_CASE("HostProcess.servesOverAdoptedEndpoints", "[vtextension][host]")
{
    auto parent = makeParentEnds();
    auto confined = std::make_shared<Confined>();
    auto const arguments = formatHelperArguments(parent.arguments);
    auto task = std::packaged_task<int()> { [&] {
        return runHost(arguments, [] { return makeHostedEngine(); }, testConfinement(confined));
    } };
    auto exitCode = task.get_future();
    auto host = std::jthread { std::move(task) };

    auto const hello = parent.control.receive();
    REQUIRE(hello.has_value());
    CHECK(*hello
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = proto::LockdownState::Unsandboxed } });
    CHECK(confined->boundTo.load() == TheParent);
    CHECK(confined->cappedTo.load() == TheDataBytes);
    CHECK(confined->confines.load() == 1);

    {
        // Contour closes the connection: the host serves no more, and ends as having served.
        auto const closed = std::move(parent.control);
    }
    REQUIRE(exitCode.wait_for(HostPatience) == std::future_status::ready);
    CHECK(exitCode.get() == exitCodeOf(HostProcessExit::Served));
    CHECK(exitCodeOf(HostProcessExit::Served) == 0);
}

namespace
{

/// Runs a host whose hardening ends as @p hardening, and checks what Hello states and when it hardened.
void checkHardening(std::expected<void, HardeningStep> hardening, proto::LockdownState expected)
{
    auto parent = makeParentEnds();
    auto confined = std::make_shared<Confined>();
    confined->hardening = hardening;
    auto const arguments = formatHelperArguments(parent.arguments);
    auto task = std::packaged_task<int()> { [&] {
        return runHost(arguments, recordingEngines(confined), testConfinement(confined));
    } };
    auto exitCode = task.get_future();
    auto host = std::jthread { std::move(task) };

    auto const hello = parent.control.receive();
    REQUIRE(hello.has_value());
    CHECK(*hello
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = expected } });
    // Hardened once, before the engine was made, and so before any thread of the host started; capped once
    // they all ran.
    CHECK(confined->hardenings.load() == 1);
    CHECK(confined->hardenedAt.load() == 0);
    CHECK(confined->engineMadeAt.load() == 1);
    CHECK(confined->confinedAt.load() == 2);
#ifdef __linux__
    // Between the two, the host started a thread per lane and its interrupt listener.
    auto const started = confined->threadsWhenConfined.load() - confined->threadsWhenHardened.load();
    CHECK(started >= MaxLaneChannels + 1);
#endif

    {
        auto const closed = std::move(parent.control);
    }
    REQUIRE(exitCode.wait_for(HostPatience) == std::future_status::ready);
    CHECK(exitCode.get() == exitCodeOf(HostProcessExit::Served));
}

/// How many times each fake hardening step ran.
std::atomic<int> firstStepRuns { 0 };
std::atomic<int> secondStepRuns { 0 };
std::atomic<int> thirdStepRuns { 0 };

} // namespace

TEST_CASE("HostProcess.hardensBeforeAnyThreadStarts", "[vtextension][host]")
{
    checkHardening({}, proto::LockdownState::Unsandboxed);
}

TEST_CASE("HostProcess.failedHardeningSaysWhichStep", "[vtextension][host]")
{
    // A hardening that failed is what Hello states, whatever the caps did; the host tells which step failed.
    auto const step = hardeningSteps().front().step;
    checkHardening(std::unexpected(step), proto::LockdownState::Failed);
    CHECK_FALSE(hardeningStepText(step).empty());
}

TEST_CASE("HostBootstrap.hardeningNamesTheFirstFailedStep", "[vtextension][host]")
{
    firstStepRuns.store(0);
    secondStepRuns.store(0);
    thirdStepRuns.store(0);
    auto const rows = std::array {
        HardeningRow { .step = HardeningStep::CoreDumps,
                       .text = "first",
                       .apply = []() noexcept -> std::expected<void, int> {
                           firstStepRuns.fetch_add(1);
                           return {};
                       } },
        HardeningRow { .step = HardeningStep::Dumpable,
                       .text = "second",
                       .apply = []() noexcept -> std::expected<void, int> {
                           secondStepRuns.fetch_add(1);
                           return std::unexpected(1);
                       } },
        HardeningRow { .step = HardeningStep::Tracing,
                       .text = "third",
                       .apply = []() noexcept -> std::expected<void, int> {
                           thirdStepRuns.fetch_add(1);
                           return std::unexpected(2);
                       } },
    };
    auto const hardened = hardenProcess(rows);
    REQUIRE_FALSE(hardened.has_value());
    CHECK(hardened.error() == rows[1].step);
    // Every step ran, past the one that failed.
    CHECK(firstStepRuns.load() == 1);
    CHECK(secondStepRuns.load() == 1);
    CHECK(thirdStepRuns.load() == 1);
}

TEST_CASE("HostBootstrap.everyStepSaysWhatFailed", "[vtextension][host]")
{
    CHECK_FALSE(hardeningSteps().empty());
    for (auto const& row: hardeningSteps())
    {
        CHECK_FALSE(row.text.empty());
        CHECK(hardeningStepText(row.step) == row.text);
    }
}

TEST_CASE("HostProcess.badArgumentsExit", "[vtextension][host]")
{
    auto engines = std::make_shared<std::atomic<int>>(0);
    auto confined = std::make_shared<Confined>();
    CHECK(runHost({ "--bogus=1" }, countingEngines(engines), testConfinement(confined)) == 70);
    CHECK(engines->load() == 0);
    CHECK(confined->boundTo.load() == 0);
}

TEST_CASE("HostProcess.unusableHandleExits", "[vtextension][host]")
{
    auto parent = makeParentEnds();
    // The control end the host is told of names nothing.
    auto arguments = parent.arguments;
    closeHandle(arguments.control);
    auto engines = std::make_shared<std::atomic<int>>(0);
    auto confined = std::make_shared<Confined>();
    CHECK(runHost(formatHelperArguments(arguments), countingEngines(engines), testConfinement(confined))
          == 71);
    CHECK(engines->load() == 0);
    // Nothing was adopted once the control end failed: the rest are still the test's.
    for (auto const lane: arguments.lanes)
        closeHandle(lane);
    closeHandle(arguments.controlPage);
    closeHandle(arguments.wake);
    // The parent's control end sees no host.
    CHECK(parent.control.receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("HostProcess.wrongParentExits", "[vtextension][host]")
{
    auto parent = makeParentEnds();
    auto arguments = parent.arguments;
    arguments.parentProcessId = ownProcessId();
    auto engines = std::make_shared<std::atomic<int>>(0);
    auto confined = std::make_shared<Confined>();
    CHECK(runHost(formatHelperArguments(arguments), countingEngines(engines), testConfinement(confined))
          == 72);
    CHECK(engines->load() == 0);
    CHECK(confined->hardenings.load() == 0);
    CHECK(confined->confines.load() == 0);
    closeAll(arguments);
}

TEST_CASE("HostProcess.noEngineExits", "[vtextension][host]")
{
    auto parent = makeParentEnds();
    auto confined = std::make_shared<Confined>();
    CHECK(runHost(
              formatHelperArguments(parent.arguments),
              [] { return std::unique_ptr<IHostedEngine> {}; },
              testConfinement(confined))
          == 73);
    CHECK(confined->hardenings.load() == 1);
    CHECK(confined->confines.load() == 0);
    // The adopted ends closed with the host.
    CHECK(parent.control.receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("HostProcess.exitCodesAreDistinct", "[vtextension][host]")
{
    auto codes = std::set<int> {};
    auto exits = std::set<HostProcessExit> {};
    for (auto const& row: hostProcessExits())
    {
        INFO(row.text);
        CHECK(codes.insert(row.code).second);
        CHECK(exits.insert(row.exit).second);
        CHECK_FALSE(std::set { 1, 23, 66 }.contains(row.code));
        CHECK_FALSE(row.text.empty());
        CHECK(exitCodeOf(row.exit) == row.code);
    }
    CHECK(exits.size() == static_cast<std::size_t>(HostProcessExit::ExecutorUnavailable) + 1);
    CHECK(exitCodeOf(HostProcessExit::Served) == 0);
}

TEST_CASE("HostProcess.serverExitsMapToProcessExits", "[vtextension][host]")
{
    CHECK(hostProcessExitOf(ServerExit::PeerClosed) == HostProcessExit::Served);
    CHECK(hostProcessExitOf(ServerExit::Stopped) == HostProcessExit::Served);
    CHECK(hostProcessExitOf(ServerExit::ProtocolViolation) == HostProcessExit::ProtocolViolation);
    CHECK(hostProcessExitOf(ServerExit::ExecutorUnavailable) == HostProcessExit::ExecutorUnavailable);
}

#if defined(__linux__) || defined(__FreeBSD__)
TEST_CASE("HostBootstrap.capResourcesApplies", "[vtextension][host]")
{
    // In a child of its own, as the caps are this process's for good. A sanitizer build's helper applies
    // none; the policy is named here, so that what it would apply is tested in every build.
    constexpr auto DataBytes = uint64_t { 256 } * 1024 * 1024;
    auto const child = ::fork();
    REQUIRE(child != -1);
    if (child == 0)
    {
        auto const lockdown = capResources(DataBytes, CapPolicy::Apply);
        auto processes = rlimit {};
        auto const nprocHolds =
            ::getrlimit(RLIMIT_NPROC, &processes) == 0 && processes.rlim_cur == 0 && processes.rlim_max == 0;
    #ifdef __linux__
        auto data = rlimit {};
        auto const dataHolds =
            ::getrlimit(RLIMIT_DATA, &data) == 0 && data.rlim_cur == DataBytes && data.rlim_max == DataBytes;
    #else
        auto const dataHolds = true;
    #endif
        ::_exit(lockdown == proto::LockdownState::Unsandboxed && nprocHolds && dataHolds ? 0 : 1);
    }
    auto status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
#endif

#ifdef __linux__
TEST_CASE("HostBootstrap.stricterInheritedLimitCountsAsCapped", "[vtextension][host]")
{
    // A hosting process that runs under a stricter data limit than the cap: the helper cannot raise the hard
    // limit it inherited, and needs not -- it stays capped at the stricter one.
    constexpr auto Inherited = uint64_t { 128 } * 1024 * 1024;
    constexpr auto DataBytes = uint64_t { 256 } * 1024 * 1024;
    auto const child = ::fork();
    REQUIRE(child != -1);
    if (child == 0)
    {
        auto const inherited = rlimit { .rlim_cur = Inherited, .rlim_max = Inherited };
        if (::setrlimit(RLIMIT_DATA, &inherited) != 0)
            ::_exit(2);
        auto const lockdown = capResources(DataBytes, CapPolicy::Apply);
        auto data = rlimit {};
        auto const dataHolds =
            ::getrlimit(RLIMIT_DATA, &data) == 0 && data.rlim_cur == Inherited && data.rlim_max == Inherited;
        ::_exit(lockdown == proto::LockdownState::Unsandboxed && dataHolds ? 0 : 1);
    }
    auto status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
#endif

TEST_CASE("HostBootstrap.skipCapsNothing", "[vtextension][host]")
{
    CHECK(capResources(TheDataBytes, CapPolicy::Skip) == proto::LockdownState::Unsandboxed);
}
