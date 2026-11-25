// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
#include <vtextension/wasm/testing/HostedEngine.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>

    #include <unistd.h>
#endif

using vtextension::wasm::host::BootstrapError;
using vtextension::wasm::host::CapPolicy;
using vtextension::wasm::host::capResources;
using vtextension::wasm::host::EngineFactory;
using vtextension::wasm::host::exitCodeOf;
using vtextension::wasm::host::HostConfinement;
using vtextension::wasm::host::HostProcessExit;
using vtextension::wasm::host::hostProcessExitOf;
using vtextension::wasm::host::hostProcessExits;
using vtextension::wasm::host::IHostedEngine;
using vtextension::wasm::host::runExtensionHost;
using vtextension::wasm::host::ServerExit;
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

/// What the tests' confinement saw.
struct Confined
{
    std::atomic<int64_t> boundTo { 0 };
    std::atomic<uint64_t> cappedTo { 0 };
    std::atomic<int> confines { 0 };
};

/// @return A confinement that touches nothing of this process: it binds to TheParent alone, and caps by
///         recording the cap.
[[nodiscard]] HostConfinement testConfinement(std::shared_ptr<Confined> const& confined)
{
    return HostConfinement {
        .bindToParent = [confined](int64_t parent) -> std::expected<void, BootstrapError> {
            confined->boundTo.store(parent);
            if (parent != TheParent)
                return std::unexpected(BootstrapError::ParentGone);
            return {};
        },
        .confine =
            [confined](uint64_t dataBytes) {
                confined->cappedTo.store(dataBytes);
                confined->confines.fetch_add(1);
                return capResources(dataBytes, CapPolicy::Skip);
            },
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

} // namespace

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

TEST_CASE("HostBootstrap.skipCapsNothing", "[vtextension][host]")
{
    CHECK(capResources(TheDataBytes, CapPolicy::Skip) == proto::LockdownState::Unsandboxed);
}
