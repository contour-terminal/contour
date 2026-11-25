// SPDX-License-Identifier: Apache-2.0
//
// The system launcher starts a real child -- extension_host_probe, which reports what it was started with --
// and the tests check what the extension host process will rely on: a clean environment, the root as its
// working directory, exactly the handles it was given, its exit seen without polling, and a kill that is
// safe however late it comes. No child outlives its case: each is killed and reaped at the end, and the reap
// is asserted. Then the test launchers the supervisor's tests stand on.
#include <core/Clock.hpp>
#include <core/testing/EnvHelper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/RecordingLauncher.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <csignal>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace vtextension::wasm::remote;
using process::ExitKind;
using process::ExitStatus;
using process::IProcessHandle;
using process::IProcessLauncher;
using process::LaunchError;
using process::LaunchSpec;
using transport::Channel;
using transport::ChannelError;
using transport::NativeHandle;

namespace
{

/// How long a case waits for a child to do what it was told.
constexpr auto ChildPatience = std::chrono::seconds { 5 };

/// @return This process's id.
[[nodiscard]] process::ProcessId ownProcessId() noexcept
{
#ifdef _WIN32
    return static_cast<process::ProcessId>(::GetCurrentProcessId());
#else
    return static_cast<process::ProcessId>(::getpid());
#endif
}

#ifdef _WIN32
/// @return The handle's value as the probe prints it.
[[nodiscard]] std::string handleText(NativeHandle handle)
{
    return std::to_string(std::bit_cast<uintptr_t>(handle));
}
#endif

/// Reaps @p process, waiting up to ChildPatience for it to exit.
/// @return Its status; nothing if it still ran when the patience ran out.
[[nodiscard]] std::optional<ExitStatus> awaitReap(IProcessLauncher& launcher, IProcessHandle& process)
{
    auto clock = core::SteadyClock {};
    auto const deadline = clock.now() + ChildPatience;
    auto set = launcher.makeWaitSet(clock);
    if (!set || !(*set)->watch(0, process))
        return std::nullopt;
    while (true)
    {
        if (auto const status = process.tryReap())
            return status;
        if (clock.now() >= deadline)
            return std::nullopt;
        if (!(*set)->wait(deadline))
            return std::nullopt;
    }
}

/// Kills @p process if it still runs, and reaps it.
/// @return Whether it is reaped: whether it is gone for good.
[[nodiscard]] bool endAndReap(IProcessLauncher& launcher, IProcessHandle& process)
{
    process.kill();
    return awaitReap(launcher, process).has_value();
}

/// What a launch of the probe needs beside the launcher's defaults.
struct ProbeOptions
{
    std::vector<std::pair<std::string, std::string>> environment {};
    std::vector<std::string> checkHandles {}; ///< Handle values the probe reports the validity of (Windows).
};

/// A running probe: the parent's ends of everything it was handed, and the process. Its destructor ends and
/// reaps a probe that a failed assertion left running.
class Probe
{
  public:
    Probe(std::shared_ptr<IProcessLauncher> launcher,
          std::unique_ptr<IProcessHandle> process,
          Channel control,
          std::vector<Channel> lanes,
          transport::ControlPage page,
          transport::WakeSender wake,
          std::shared_ptr<std::vector<NativeHandle>> childHandles) noexcept:
        launcher { std::move(launcher) },
        process { std::move(process) },
        control { std::move(control) },
        lanes { std::move(lanes) },
        page { std::move(page) },
        wake { std::move(wake) },
        childHandles { std::move(childHandles) }
    {
    }
    Probe(Probe&&) noexcept = default;
    Probe& operator=(Probe&&) noexcept = delete;
    Probe(Probe const&) = delete;
    Probe& operator=(Probe const&) = delete;
    ~Probe()
    {
        if (process)
            std::ignore = endAndReap(*launcher, *process);
    }

    std::shared_ptr<IProcessLauncher> launcher;
    std::unique_ptr<IProcessHandle> process;
    Channel control;
    std::vector<Channel> lanes;
    transport::ControlPage page;
    transport::WakeSender wake;
    std::shared_ptr<std::vector<NativeHandle>> childHandles; ///< What the arguments callback was given.

    /// @return The probe's report, one line each, once it wrote all of it; nothing if the channel failed
    /// first.
    [[nodiscard]] std::optional<std::vector<std::string>> report()
    {
        auto text = std::string {};
        auto buffer = std::array<uint8_t, 4096> {};
        while (!text.ends_with("end\n"))
        {
            auto const received = control.receiveSome(buffer);
            if (!received)
                return std::nullopt;
            text.append(reinterpret_cast<char const*>(buffer.data()), *received);
        }
        auto lines = std::vector<std::string> {};
        for (auto const line: std::views::split(std::string_view { text }, '\n'))
            if (!line.empty())
                lines.emplace_back(std::string_view { line });
        return lines;
    }

    /// Ends the probe the way a helper ends: its control channel reaches end of stream.
    void closeControl() noexcept
    {
        control.shutdown();
        auto const closed = std::move(control);
    }
};

/// @return The values of @p report's lines that start with @p key, sorted.
[[nodiscard]] std::vector<std::string> valuesOf(std::vector<std::string> const& report, std::string_view key)
{
    auto values = std::vector<std::string> {};
    for (auto const& line: report)
        if (line.starts_with(key))
            values.push_back(line.substr(key.size()));
    std::ranges::sort(values);
    return values;
}

/// Launches the probe with fresh channels, page and wake.
[[nodiscard]] std::expected<Probe, LaunchError> launchProbe(std::shared_ptr<IProcessLauncher> launcher,
                                                            ProbeOptions options = {})
{
    auto control = transport::makeChannelPair();
    REQUIRE(control.has_value());
    auto lanes = std::vector<transport::ChannelPair> {};
    for ([[maybe_unused]] auto const i: std::views::iota(std::size_t { 0 }, MaxLaneChannels))
    {
        auto lane = transport::makeChannelPair();
        REQUIRE(lane.has_value());
        lanes.push_back(*std::move(lane));
    }
    auto page = transport::ControlPage::create();
    REQUIRE(page.has_value());
    auto wake = transport::makeWakePair();
    REQUIRE(wake.has_value());

    auto spec = LaunchSpec { .executable = VTEXTENSION_PROBE_PATH,
                             .inherited = {},
                             .arguments = {},
                             .environment = std::move(options.environment),
                             .caps = {} };
    spec.inherited.push_back(control->helper.nativeHandle());
    for (auto const& lane: lanes)
        spec.inherited.push_back(lane.helper.nativeHandle());
    spec.inherited.push_back(page->nativeHandle());
    spec.inherited.push_back(wake->helper.nativeHandle());

    auto childHandles = std::make_shared<std::vector<NativeHandle>>();
    spec.arguments = [childHandles,
                      checkHandles = std::move(options.checkHandles)](std::span<NativeHandle const> handles) {
        childHandles->assign(handles.begin(), handles.end());
        auto arguments = transport::HelperArguments { .control = handles[0],
                                                      .lanes = {},
                                                      .controlPage = handles[1 + MaxLaneChannels],
                                                      .wake = handles[2 + MaxLaneChannels],
                                                      .parentProcessId = ownProcessId(),
                                                      .dataBytes = process::DefaultDataBytes };
        std::ranges::copy(handles.subspan(1, MaxLaneChannels), arguments.lanes.begin());
        auto result = std::vector<std::string> { "--report" };
        std::ranges::copy(transport::formatHelperArguments(arguments), std::back_inserter(result));
#ifdef _WIN32
        // Every handle the probe was given must be open in it.
        for (auto const handle: handles)
            result.push_back(std::format("--check-handle={}", handleText(handle)));
#endif
        for (auto const& handle: checkHandles)
            result.push_back(std::format("--check-handle={}", handle));
        return result;
    };

    auto process = launcher->launch(spec);
    if (!process)
        return std::unexpected(process.error());
    // The helper's ends are the child's alone now: the parent's copies close here, as a connector's do when
    // its spec goes out of scope.
    auto parentLanes = std::vector<Channel> {};
    for (auto& lane: lanes)
        parentLanes.push_back(std::move(lane.parent));
    return Probe { std::move(launcher),    *std::move(process), std::move(control->parent),
                   std::move(parentLanes), *std::move(page),    std::move(wake->parent),
                   std::move(childHandles) };
}

} // namespace

TEST_CASE("ProcessLauncher.childGetsACleanEnvironment", "[vtextension]")
{
    auto const leak = core::testing::ScopedEnv { "VTEXTENSION_LEAK", "1" };
    auto probe =
        launchProbe(process::makeSystemProcessLauncher(), ProbeOptions { .environment = { { "A", "1" } } });
    REQUIRE(probe.has_value());
    auto const report = probe->report();
    REQUIRE(report.has_value());
    auto names = valuesOf(*report, "env=");
#ifdef _WIN32
    // Wine adds its own variables to every process it starts, whatever block it was given; Windows adds none.
    if (::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr)
        std::erase_if(names, [](std::string const& name) {
            return name.starts_with("WINE") || name == "SystemDrive" || name == "SystemRoot"
                   || name == "PROCESSOR_ARCHITECTURE";
        });
#endif
    CHECK(names == std::vector<std::string> { "A" });
    CHECK(endAndReap(*probe->launcher, *probe->process));
}

TEST_CASE("ProcessLauncher.childRunsAtTheRootOnTheNullDevice", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    auto const report = probe->report();
    REQUIRE(report.has_value());
#ifdef _WIN32
    auto const root = std::filesystem::absolute(VTEXTENSION_PROBE_PATH).root_path().string();
#else
    auto const root = std::string { "/" };
#endif
    CHECK(valuesOf(*report, "cwd=") == std::vector<std::string> { root });
    CHECK(valuesOf(*report, "stdin=") == std::vector<std::string> { "null" });
    CHECK(valuesOf(*report, "stdout=") == std::vector<std::string> { "null" });
    CHECK(endAndReap(*probe->launcher, *probe->process));
}

TEST_CASE("ProcessLauncher.childInheritsExactlyItsHandles", "[vtextension]")
{
#ifdef _WIN32
    // Inheritable, but not handed over: the handle list keeps it out of the child. Made after many others, so
    // that its value is above any the child opens of its own, which would answer for it.
    constexpr auto Fillers = 512;
    auto fillers = std::vector<HANDLE> {};
    for ([[maybe_unused]] auto const i: std::views::iota(0, Fillers))
        fillers.push_back(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    auto attributes = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                            .lpSecurityDescriptor = nullptr,
                                            .bInheritHandle = TRUE };
    auto* const stray = ::CreateEventW(&attributes, TRUE, FALSE, nullptr);
    for (auto* const filler: fillers)
        ::CloseHandle(filler);
    REQUIRE(stray != nullptr);
    auto probe = launchProbe(process::makeSystemProcessLauncher(),
                             ProbeOptions { .checkHandles = { handleText(stray) } });
    REQUIRE(probe.has_value());
    auto const report = probe->report();
    ::CloseHandle(stray);
    REQUIRE(report.has_value());
    auto expected = std::vector<std::string> {};
    for (auto const handle: *probe->childHandles)
        expected.push_back(std::format("{}:valid", handleText(handle)));
    expected.push_back(std::format("{}:invalid", handleText(stray)));
    std::ranges::sort(expected);
    CHECK(valuesOf(*report, "handle=") == expected);
#else
    // Not close-on-exec, and above every number the child is given: only the launcher's own closing keeps it
    // out of the child. (A low number would be hidden by the dup2() onto it.)
    constexpr auto StrayAtLeast = 32;
    auto const nullDevice = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    REQUIRE(nullDevice != -1);
    auto const stray = ::fcntl(nullDevice, F_DUPFD, StrayAtLeast);
    ::close(nullDevice);
    REQUIRE(stray >= StrayAtLeast);
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    ::close(stray);
    REQUIRE(probe.has_value());
    auto const report = probe->report();
    REQUIRE(report.has_value());
    auto expected = std::vector<std::string> {};
    for (auto const descriptor: std::views::iota(0, 10))
        expected.push_back(std::to_string(descriptor));
    std::ranges::sort(expected);
    CHECK(valuesOf(*report, "fd=") == expected);
#endif
    CHECK(endAndReap(*probe->launcher, *probe->process));
}

#ifndef _WIN32
TEST_CASE("ProcessLauncher.childHandlesHaveFixedNumbers", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    CHECK(*probe->childHandles == std::vector<NativeHandle> { 3, 4, 5, 6, 7, 8, 9 });
    CHECK(endAndReap(*probe->launcher, *probe->process));
}
#endif

TEST_CASE("ProcessLauncher.exitIsSeenWithoutPolling", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    auto clock = core::SteadyClock {};
    auto set = probe->launcher->makeWaitSet(clock);
    REQUIRE(set.has_value());
    REQUIRE((*set)->watch(7, *probe->process).has_value());
    REQUIRE(probe->report().has_value());

    probe->closeControl();
    CHECK((*set)->wait(std::nullopt) == std::vector<uint64_t> { 7 });
    CHECK(probe->process->tryReap() == ExitStatus { .kind = ExitKind::Exited, .value = 0 });
    (*set)->unwatch(7);
}

TEST_CASE("ProcessLauncher.killThenReap", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    auto clock = core::SteadyClock {};
    auto set = probe->launcher->makeWaitSet(clock);
    REQUIRE(set.has_value());
    REQUIRE((*set)->watch(3, *probe->process).has_value());
    REQUIRE(probe->report().has_value());
    CHECK_FALSE(probe->process->tryReap().has_value());

    probe->process->kill();
    CHECK((*set)->wait(clock.now() + ChildPatience) == std::vector<uint64_t> { 3 });
#ifdef _WIN32
    auto const killed =
        ExitStatus { .kind = ExitKind::Exited, .value = static_cast<int>(process::KilledExitCode) };
#else
    auto const killed = ExitStatus { .kind = ExitKind::Signalled, .value = SIGKILL };
#endif
    CHECK(probe->process->tryReap() == killed);
}

TEST_CASE("ProcessLauncher.killAfterReapDoesNothing", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    REQUIRE(probe->report().has_value());
    probe->closeControl();
    auto const first = awaitReap(*probe->launcher, *probe->process);
    REQUIRE(first == ExitStatus { .kind = ExitKind::Exited, .value = 0 });

    // Reaped: a kill now could reach whatever process took the id over, so it reaches nothing.
    probe->process->kill();
    probe->process->kill();
    CHECK(probe->process->tryReap() == first);
}

TEST_CASE("ProcessLauncher.missingExecutableIsNotFound", "[vtextension]")
{
    auto const launcher = process::makeSystemProcessLauncher();
    auto const spec = LaunchSpec { .executable = "/nonexistent/x",
                                   .inherited = {},
                                   .arguments = [](auto) { return std::vector<std::string> {}; },
                                   .environment = {},
                                   .caps = {} };
    CHECK(launcher->launch(spec) == std::unexpected(LaunchError::NotFound));
}

#ifdef _WIN32
TEST_CASE("ProcessLauncher.childIsInAKillOnCloseJob", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    REQUIRE(probe->report().has_value());
    auto inJob = BOOL { FALSE };
    REQUIRE(::IsProcessInJob(probe->process->waitable(), nullptr, &inJob));
    CHECK(inJob == TRUE);

    auto* const watcher = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(probe->process->id()));
    REQUIRE(watcher != nullptr);
    // Neither killed nor waited for: the job's close ends it.
    probe->process.reset();
    CHECK(::WaitForSingleObject(watcher, 5000) == WAIT_OBJECT_0);
    ::CloseHandle(watcher);
}
#endif

TEST_CASE("ExitWaitSet.wakeEndsAWait", "[vtextension]")
{
    auto const launcher = process::makeSystemProcessLauncher();
    auto clock = core::SteadyClock {};
    auto set = launcher->makeWaitSet(clock);
    REQUIRE(set.has_value());
    using WaitResult = std::expected<std::vector<uint64_t>, LaunchError>;
    auto result = std::promise<WaitResult> {};
    auto ended = result.get_future();
    auto waiter = std::jthread { [&] { result.set_value((*set)->wait(std::nullopt)); } };
    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
    (*set)->wake();
    REQUIRE(ended.wait_for(ChildPatience) == std::future_status::ready);
    CHECK(ended.get() == std::vector<uint64_t> {});
}

TEST_CASE("ExitWaitSet.deadlineEndsAWait", "[vtextension]")
{
    auto const launcher = process::makeSystemProcessLauncher();
    auto clock = core::SteadyClock {};
    auto set = launcher->makeWaitSet(clock);
    REQUIRE(set.has_value());
    auto const start = clock.now();
    CHECK((*set)->wait(start + std::chrono::milliseconds { 20 }) == std::vector<uint64_t> {});
    CHECK(clock.now() - start >= std::chrono::milliseconds { 20 });
}

#if defined(__linux__) || defined(__FreeBSD__)
TEST_CASE("ExitWaitSet.closedWatchedHandleIsAnError", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    auto clock = core::SteadyClock {};
    auto set = probe->launcher->makeWaitSet(clock);
    REQUIRE(set.has_value());
    REQUIRE((*set)->watch(5, *probe->process).has_value());

    // The process descriptor closes out from under the set; a copy keeps the process's own handle restorable.
    auto const descriptor = probe->process->waitable();
    auto const saved = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    REQUIRE(saved >= 0);
    REQUIRE(::close(descriptor) == 0);
    auto const waited = (*set)->wait(clock.now() + ChildPatience);
    REQUIRE(::dup3(saved, descriptor, O_CLOEXEC) == descriptor);
    ::close(saved);

    CHECK_FALSE(waited.has_value());
    (*set)->unwatch(5);
}
#endif

TEST_CASE("Channel.peerProcessExitIsClosed", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    REQUIRE(probe->report().has_value());
    // The lanes' other ends exist only in the probe: its exit ends their streams.
    probe->closeControl();
    auto buffer = std::array<uint8_t, 16> {};
    CHECK(probe->lanes.front().receiveSome(buffer) == std::unexpected(ChannelError::Closed));
    CHECK(endAndReap(*probe->launcher, *probe->process));
}

TEST_CASE("FakeProcessLauncher.recordsAndExits", "[vtextension]")
{
    auto const launcher = std::make_shared<process::testing::FakeProcessLauncher>();
    auto clock = core::ManualClock {};
    auto set = launcher->makeWaitSet(clock);
    REQUIRE(set.has_value());

    auto const spec = LaunchSpec { .executable = "helper",
                                   .inherited = {},
                                   .arguments = [](auto) { return std::vector<std::string> { "--x" }; },
                                   .environment = { { "A", "1" } },
                                   .caps = {} };
    auto first = launcher->launch(spec);
    auto second = launcher->launch(spec);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK((*first)->id() != (*second)->id());
    REQUIRE(launcher->launches().size() == 2);
    CHECK(launcher->launches()[1].executable == "helper");
    CHECK(launcher->launchThreads()
          == std::vector { std::this_thread::get_id(), std::this_thread::get_id() });
    REQUIRE((*set)->watch(10, **first).has_value());
    REQUIRE((*set)->watch(20, **second).has_value());

    SECTION("an exit wakes the set and is reaped")
    {
        auto result = std::promise<std::expected<std::vector<uint64_t>, LaunchError>> {};
        auto woken = result.get_future();
        auto const deadline = clock.now() + std::chrono::seconds { 1 };
        auto waiter = std::jthread { [&] { result.set_value((*set)->wait(deadline)); } };
        // Real time does not end its wait, and neither does the deadline: only an exit or a wake does.
        CHECK(woken.wait_for(std::chrono::milliseconds { 50 }) == std::future_status::timeout);
        launcher->exit(1, ExitStatus { .kind = ExitKind::Exited, .value = 3 });
        CHECK(woken.get() == std::vector<uint64_t> { 20 });
        CHECK(launcher->lastDeadline() == deadline);
        CHECK_FALSE((*first)->tryReap().has_value());
        CHECK((*second)->tryReap() == ExitStatus { .kind = ExitKind::Exited, .value = 3 });
    }
    SECTION("a kill is recorded and ends the process")
    {
        (*first)->kill();
        (*first)->kill();
        CHECK(launcher->kills() == std::vector<std::size_t> { 0, 0 });
        CHECK((*set)->wait(std::nullopt) == std::vector<uint64_t> { 10 });
        CHECK((*first)->tryReap() == process::testing::FakeKilledStatus);
    }
    SECTION("a wake ends a wait with nothing")
    {
        (*set)->wake();
        CHECK((*set)->wait(std::nullopt) == std::vector<uint64_t> {});
    }
}

TEST_CASE("RecordingLauncher.recordsAndRewritesLaterLaunches", "[vtextension]")
{
    auto const inner = std::make_shared<process::testing::FakeProcessLauncher>();
    auto const recording =
        std::make_shared<process::testing::RecordingLauncher>(inner, [](std::vector<std::string> arguments) {
            arguments.emplace_back("--later");
            return arguments;
        });
    auto const spec = LaunchSpec { .executable = "helper",
                                   .inherited = {},
                                   .arguments = [](auto) { return std::vector<std::string> { "--x" }; },
                                   .environment = {},
                                   .caps = {} };
    auto first = recording->launch(spec);
    auto second = recording->launch(spec);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK(recording->launches() == 2);
    CHECK(recording->processIds() == std::vector { (*first)->id(), (*second)->id() });
    auto const noHandles = std::span<NativeHandle const> {};
    CHECK(inner->launches()[0].arguments(noHandles) == std::vector<std::string> { "--x" });
    CHECK(inner->launches()[1].arguments(noHandles) == std::vector<std::string> { "--x", "--later" });

    auto killerId = std::thread::id {};
    auto killer = std::jthread { [&] {
        killerId = std::this_thread::get_id();
        (*second)->kill();
    } };
    killer.join();
    CHECK(recording->killThreads() == std::vector { killerId });
    CHECK(inner->kills() == std::vector<std::size_t> { 1 });
}
