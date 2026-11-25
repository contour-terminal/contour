// SPDX-License-Identifier: Apache-2.0
//
// The system launcher starts a real child -- extension_host_probe, which reports what it was started with --
// and the tests check what the extension host process will rely on: a clean environment, the root as its
// working directory, exactly the handles it was given, its exit seen without polling, and a kill that is
// safe however late it comes. No child outlives its case: each is killed and reaped at the end, and the reap
// is asserted. Then the test launchers the supervisor's tests stand on.
#include <core/Clock.hpp>
#include <core/Utils.hpp>
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

#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#ifdef __linux__
    #include <vtextension/wasm/remote/process/Process_linux.hpp>
#endif
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
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
    #include <poll.h>
    #include <unistd.h>
#endif

using namespace vtextension::wasm::remote;
using process::ExitKind;
using process::ExitStatus;
using process::IProcessHandle;
using process::IProcessLauncher;
using process::LaunchError;
using process::LaunchSpec;
using transport::ChannelError;
using transport::NativeHandle;

namespace
{

using process::testing::awaitReap;
using process::testing::ChildPatience;
using process::testing::endAndReap;
using process::testing::ownProcessId;
using process::testing::readReport;
using Probe = process::testing::LaunchedHelper;

#ifdef _WIN32
/// @return The handle's value as the probe prints it.
[[nodiscard]] std::string handleText(NativeHandle handle)
{
    return std::to_string(std::bit_cast<uintptr_t>(handle));
}
#endif

/// The data cap the probe is told of, and caps itself to.
constexpr auto ProbeDataBytes = uint64_t { 64 } * 1024 * 1024;

/// What a launch of the probe needs beside the launcher's defaults.
struct ProbeOptions
{
    std::vector<std::pair<std::string, std::string>> environment {};
    std::vector<std::string> checkHandles {}; ///< Handle values the probe reports the validity of (Windows).
    process::ProcessId parentProcessId = ownProcessId(); ///< What `--parent` names.
    /// Its stderr: this process's own, so that what it reports shows.
    std::optional<NativeHandle> standardError = process::testing::ownStandardError();
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
    return process::testing::launchHelper(
        std::move(launcher),
        process::testing::HelperLaunch {
            .executable = VTEXTENSION_PROBE_PATH,
            .leading = { "--report" },
            .trailing =
                [checkHandles = std::move(options.checkHandles)](
                    [[maybe_unused]] std::span<NativeHandle const> handles) {
                    auto result = std::vector<std::string> {};
#ifdef _WIN32
                    // Every handle the probe was given must be open in it.
                    for (auto const handle: handles)
                        result.push_back(std::format("--check-handle={}", handleText(handle)));
#endif
                    for (auto const& handle: checkHandles)
                        result.push_back(std::format("--check-handle={}", handle));
                    return result;
                },
            .parentProcessId = options.parentProcessId,
            .dataBytes = ProbeDataBytes,
            .environment = std::move(options.environment),
            .standardError = options.standardError });
}

} // namespace

TEST_CASE("ProcessLauncher.childGetsACleanEnvironment", "[vtextension]")
{
    auto const leak = core::testing::ScopedEnv { "VTEXTENSION_LEAK", "1" };
    auto probe =
        launchProbe(process::makeSystemProcessLauncher(), ProbeOptions { .environment = { { "A", "1" } } });
    REQUIRE(probe.has_value());
    auto const report = readReport(probe->control);
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
    auto const report = readReport(probe->control);
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

TEST_CASE("ProcessLauncher.childStderrIsTheNullDeviceUnlessGiven", "[vtextension]")
{
    // Nothing of this process's terminal reaches a child that was given no stderr.
    auto probe =
        launchProbe(process::makeSystemProcessLauncher(), ProbeOptions { .standardError = std::nullopt });
    REQUIRE(probe.has_value());
    auto const report = readReport(probe->control);
    REQUIRE(report.has_value());
    CHECK(valuesOf(*report, "stderr=") == std::vector<std::string> { "null" });
    CHECK(endAndReap(*probe->launcher, *probe->process));
}

#ifndef _WIN32
TEST_CASE("ProcessLauncher.childLeadsASessionOfItsOwn", "[vtextension]")
{
    // A session of its own has no controlling terminal: the child can neither open this process's terminal
    // nor be signalled by it.
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    auto const report = readReport(probe->control);
    REQUIRE(report.has_value());
    auto const session = valuesOf(*report, "sid=");
    REQUIRE(session.size() == 1);
    CHECK(session.front() == std::to_string(probe->process->id()));
    CHECK(session.front() != std::to_string(::getsid(0)));
    CHECK(endAndReap(*probe->launcher, *probe->process));
}
#endif

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
    auto const report = readReport(probe->control);
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
    auto const report = readReport(probe->control);
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
    REQUIRE(readReport(probe->control).has_value());

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
    REQUIRE(readReport(probe->control).has_value());
    CHECK(probe->process->tryReap() == std::nullopt);

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
    REQUIRE(readReport(probe->control).has_value());
    probe->closeControl();
    auto const first = awaitReap(*probe->launcher, *probe->process);
    REQUIRE(first == ExitStatus { .kind = ExitKind::Exited, .value = 0 });

    // Reaped: a kill now could reach whatever process took the id over, so it reaches nothing.
    probe->process->kill();
    probe->process->kill();
    auto const again = probe->process->tryReap();
    REQUIRE(again.has_value());
    CHECK(*again == first);
}

TEST_CASE("LaunchErrors.oneRowPerError", "[vtextension]")
{
    auto const rows = process::launchErrors();
    CHECK(rows.size() == std::size_t { std::to_underlying(LaunchError::Io) } + 1);
    for (auto const& [index, row]: core::views::enumerate(rows))
    {
        INFO(row.text);
        CHECK(std::size_t { std::to_underlying(row.error) } == index);
        CHECK_FALSE(row.text.empty());
    }
}

TEST_CASE("BuildInstrumentation.drivesCapsAndStderr", "[vtextension]")
{
    using vtextension::wasm::BuildInstrumentation;
    using vtextension::wasm::ThisBuildInstrumentation;
    using vtextension::wasm::host::BuildCapPolicy;
    using vtextension::wasm::host::CapPolicy;

    // A sanitizer's shadow memory and threads exceed any cap; only there is the host left uncapped.
    auto const sanitized = ThisBuildInstrumentation == BuildInstrumentation::Sanitizers;
    CHECK((BuildCapPolicy == CapPolicy::Skip) == sanitized);
    // An instrumented build's helpers report to Contour's stderr; any other's to the null device.
    auto const instrumented = ThisBuildInstrumentation != BuildInstrumentation::None;
    CHECK(process::defaultHelperStandardError().has_value() == instrumented);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    CHECK(sanitized);
#elifdef __has_feature
    #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    CHECK(sanitized);
    #endif
#endif
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

#ifdef __linux__
TEST_CASE("ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch", "[vtextension]")
{
    auto const launcher = process::makeLinuxProcessLauncher(process::PidfdWaits::Unsupported);
    // Everything the probe needs to write its report, so that a child started anyway would write one.
    auto prepared = process::testing::prepareHelperLaunch(process::testing::HelperLaunch {
        .executable = VTEXTENSION_PROBE_PATH, .leading = { "--report" }, .dataBytes = ProbeDataBytes });
    // Kept alive to the end, so that a child started anyway is neither killed nor reaped before it reported.
    auto launched = launcher->launch(prepared.spec);
    CHECK(launched == std::unexpected(LaunchError::Unsupported));
    // Refused before anything started: once Contour lets go of the helper's ends, no child holds them, and
    // the control channel ends at once -- rather than carry a report, or stay open past the patience.
    auto control = std::move(prepared.control.parent);
    {
        auto const dropped = std::move(prepared);
    }
    auto readable = pollfd { .fd = control.nativeHandle(), .events = POLLIN, .revents = 0 };
    auto const patience = std::chrono::duration_cast<std::chrono::milliseconds>(ChildPatience);
    auto const ready = ::poll(&readable, 1, static_cast<int>(patience.count()));
    CHECK(ready == 1);
    // A child started anyway is ended and reaped whatever the poll said: the handle does not kill it.
    if (launched)
        CHECK(endAndReap(*launcher, **launched));
    if (ready != 1)
        return;
    auto buffer = std::array<uint8_t, 16> {};
    CHECK(control.receiveSome(buffer) == std::unexpected(ChannelError::Closed));
}

TEST_CASE("ProcessLauncher.thisKernelWaitsOnPidfds", "[vtextension]")
{
    // Every kernel this is tested on is 5.4 or later.
    CHECK(process::probePidfdWaits() == process::PidfdWaits::Supported);
}
#endif

#ifdef _WIN32
TEST_CASE("ProcessLauncher.childIsInAKillOnCloseJob", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    REQUIRE(readReport(probe->control).has_value());
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
namespace
{

/// A process handle whose exit is waited on by a descriptor of the test's own: a copy of the process's,
/// which the test may close without touching the handle the process is owned by.
class CopiedWaitable final: public IProcessHandle
{
  public:
    CopiedWaitable(IProcessHandle& process, int copy) noexcept: _process { process }, _copy { copy } {}
    [[nodiscard]] process::ProcessId id() const noexcept override { return _process.id(); }
    void kill() noexcept override { _process.kill(); }
    [[nodiscard]] std::expected<std::optional<ExitStatus>, LaunchError> tryReap() noexcept override
    {
        return _process.tryReap();
    }
    [[nodiscard]] process::Waitable waitable() const noexcept override { return _copy; }

    [[nodiscard]] std::expected<transport::HandleTarget, LaunchError> handleTarget() override
    {
        return _process.handleTarget();
    }

  private:
    IProcessHandle& _process;
    int _copy;
};

} // namespace

TEST_CASE("ExitWaitSet.closedWatchedHandleIsAnError", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    auto clock = core::SteadyClock {};
    auto set = probe->launcher->makeWaitSet(clock);
    REQUIRE(set.has_value());
    auto const copy = ::fcntl(probe->process->waitable(), F_DUPFD_CLOEXEC, 0);
    REQUIRE(copy >= 0);
    auto watched = CopiedWaitable { *probe->process, copy };
    auto const watching = (*set)->watch(5, watched);
    if (!watching)
        ::close(copy); // the case ends at the REQUIRE below; the copy must not outlive it
    REQUIRE(watching.has_value());

    // The watched descriptor closes out from under the set. It is the test's copy, so the process's own
    // handle stays whole, and nothing ever closes the number again.
    REQUIRE(::close(copy) == 0);
    auto const waited = (*set)->wait(clock.now() + ChildPatience);
    (*set)->unwatch(5);
    CHECK_FALSE(waited.has_value());
}
#endif

TEST_CASE("Channel.peerProcessExitIsClosed", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    REQUIRE(readReport(probe->control).has_value());
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
        CHECK((*first)->tryReap() == std::nullopt);
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

TEST_CASE("HostBootstrap.capsHold", "[vtextension]")
{
    auto probe = launchProbe(process::makeSystemProcessLauncher());
    REQUIRE(probe.has_value());
    auto const report = readReport(probe->control);
    REQUIRE(report.has_value());
    probe->closeControl();
    CHECK(awaitReap(*probe->launcher, *probe->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 0 });

    if (valuesOf(*report, "caps=") == std::vector<std::string> { "skip" })
        SKIP("sanitizer builds apply no caps");
    CHECK(valuesOf(*report, "caps=") == std::vector<std::string> { "apply" });
#ifdef __linux__
    CHECK(valuesOf(*report, "nproc=") == std::vector<std::string> { "0" });
    CHECK(valuesOf(*report, "data=") == std::vector<std::string> { std::to_string(ProbeDataBytes) });
#elifdef __FreeBSD__
    // FreeBSD reads a process limit of 0 as none at all, so 1, which the user's count is at already.
    CHECK(valuesOf(*report, "nproc=") == std::vector<std::string> { "1" });
    CHECK(valuesOf(*report, "as=")
          == std::vector<std::string> {
              std::to_string(vtextension::wasm::host::addressSpaceCap(ProbeDataBytes)) });
#endif
}

#ifndef _WIN32
TEST_CASE("HostBootstrap.wrongParentExits", "[vtextension]")
{
    // The process the probe is told of is not its parent: it ends before it adopts anything.
    constexpr auto ParentGoneExitCode = 72;
    auto probe = launchProbe(process::makeSystemProcessLauncher(), ProbeOptions { .parentProcessId = 1 });
    REQUIRE(probe.has_value());
    CHECK(awaitReap(*probe->launcher, *probe->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = ParentGoneExitCode });
}
#endif
