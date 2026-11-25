// SPDX-License-Identifier: Apache-2.0
//
// Letting go of extension host processes: nothing the owner thread drops waits for a helper, the supervisor
// does the killing and the reaping on its own thread, a quitting supervisor is bounded however its helpers
// behave, a helper given a teardown grace exits on its own, and no helper outlives the process that started
// it. Real executables throughout; each case composes its own supervisor, so that its counts are its own.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/types.h>
    #include <sys/wait.h>

    #include <cerrno>
    #include <csignal>
    #ifdef __linux__
        #include <sys/prctl.h>
    #elifdef __FreeBSD__
        #include <sys/procctl.h>
    #endif
#endif

namespace process = vtextension::wasm::remote::process;
namespace contract = vtextension::wasm::testing::contract;
using process::KillReason;
using process::ProcessId;
using process::testing::HelperBackendConfig;
using process::testing::HelperComposition;
using vtextension::ExtensionError;
using vtextension::testing::toModule;
using vtextension::wasm::HostTimeouts;
using vtextension::wasm::IWasmInstance;
using vtextension::wasm::LockdownRequirement;
using vtextension::wasm::ModuleIdentity;
using namespace std::chrono_literals;

namespace
{

/// How long any one thing a case waits for may take; it fails, rather than hang, beyond it.
constexpr auto Patience = std::chrono::seconds { 5 };

/// How long the owner thread may take to drop what it holds: a drop that waited for a helper would not make
/// it, as a helper that stops reading never answers.
constexpr auto DropBound = std::chrono::seconds { 1 };

/// How long a helper whose parent was killed may outlive it.
constexpr auto OrphanBound = std::chrono::seconds { 2 };

/// The module a load that a host stops reading blocks on: larger than any channel's buffer.
constexpr auto LargeModuleBytes = std::size_t { 16 } * 1024 * 1024;

/// Ends the test run, rather than wait forever on a thread or a future that a regression keeps from
/// finishing: their destructors would join or wait.
[[noreturn]] void abandon(char const* what)
{
    FAIL_CHECK(what);
    std::println(stderr, "Teardown: {}", what);
    std::abort();
}

/// @return @p future's value, once ready within @p bound; else the run ends (abandon()).
template <typename T>
[[nodiscard]] T within(std::future<T>& future, std::chrono::milliseconds bound, char const* what)
{
    if (future.wait_for(bound) != std::future_status::ready)
        abandon(what);
    return future.get();
}

/// @return A helper of @p path, hostile as @p hostile names unless that is empty, held to @p timeouts.
[[nodiscard]] HelperBackendConfig helperOf(char const* path,
                                           std::string_view hostile,
                                           HostTimeouts timeouts = {})
{
    auto config = HelperBackendConfig { .helper = path,
                                        .extraArguments = {},
                                        .lockdown = LockdownRequirement::AcceptUnsandboxed,
                                        .timeouts = timeouts };
    if (!hostile.empty())
        config.extraArguments.push_back(std::format("--hostile={}", hostile));
    return config;
}

/// @return Whether every kill was sent from @p thread; some were.
[[nodiscard]] bool everyKillRanOn(process::testing::RecordingLauncher const& recording,
                                  std::thread::id thread)
{
    auto const threads = recording.killThreads();
    return !threads.empty()
           && std::ranges::all_of(threads, [&](std::thread::id killer) { return killer == thread; });
}

#ifdef _WIN32
/// A process, as a handle that signals once it ended; opened before it could end, so that its id cannot name
/// another process by then.
class ProcessWatch
{
  public:
    explicit ProcessWatch(ProcessId processId):
        _handle { ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(processId)) }
    {
    }
    ProcessWatch(ProcessWatch const&) = delete;
    ProcessWatch(ProcessWatch&&) = delete;
    ProcessWatch& operator=(ProcessWatch const&) = delete;
    ProcessWatch& operator=(ProcessWatch&&) = delete;
    ~ProcessWatch()
    {
        if (_handle != nullptr)
            ::CloseHandle(_handle);
    }

    /// @return Whether the process ended within @p bound.
    [[nodiscard]] bool endsWithin(std::chrono::milliseconds bound) const
    {
        return _handle != nullptr
               && ::WaitForSingleObject(_handle, static_cast<DWORD>(bound.count())) == WAIT_OBJECT_0;
    }

  private:
    HANDLE _handle;
};
#else
/// @return Whether process @p processId, a child of this process, ended within @p bound: reaped here, or by
///         whoever reaped it before (its id names nothing then).
[[nodiscard]] bool childEndsWithin(ProcessId processId, std::chrono::milliseconds bound)
{
    auto const pid = static_cast<pid_t>(processId);
    auto const deadline = std::chrono::steady_clock::now() + bound;
    while (true)
    {
        auto status = 0;
        auto const reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == pid || (reaped == -1 && errno == ECHILD && ::kill(pid, 0) == -1 && errno == ESRCH))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
}

/// Kills and reaps process @p processId, a child of this process, if a failed case left it running.
void killChild(ProcessId processId)
{
    auto const pid = static_cast<pid_t>(processId);
    if (::kill(pid, SIGKILL) == 0)
    {
        auto status = 0;
        std::ignore = ::waitpid(pid, &status, 0);
    }
}
#endif

/// Makes this process the reaper of the processes its children leave behind, once for the binary: an
/// orphaned helper is then this process's child, which it can wait for (Linux, FreeBSD).
void becomeSubreaper()
{
#ifdef __linux__
    static auto const isSubreaper = ::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0;
    REQUIRE(isSubreaper);
#elifdef __FreeBSD__
    static auto const isSubreaper = ::procctl(P_PID, ::getpid(), PROC_REAP_ACQUIRE, nullptr) == 0;
    REQUIRE(isSubreaper);
#endif
}

/// @return The process id in the `helper=<pid>` line a probe started with --parent sends on @p control;
///         nothing if the channel ended without one.
[[nodiscard]] std::optional<ProcessId> helperReported(vtextension::wasm::remote::transport::Channel& control)
{
    auto text = std::string {};
    auto buffer = std::array<uint8_t, 64> {};
    while (!text.contains("end\n"))
    {
        auto const received = control.receiveSome(buffer);
        if (!received)
            return std::nullopt;
        text.append(reinterpret_cast<char const*>(buffer.data()), *received);
    }
    constexpr auto Prefix = std::string_view { "helper=" };
    auto const start = text.find(Prefix);
    if (start == std::string::npos)
        return std::nullopt;
    auto processId = ProcessId {};
    auto const digits = std::string_view { text }.substr(start + Prefix.size());
    if (std::from_chars(digits.data(), digits.data() + digits.size(), processId).ec != std::errc {})
        return std::nullopt;
    return processId;
}

} // namespace

TEST_CASE("teardown never blocks the owner thread", "[vtextension][process][threading]")
{
    SECTION("closing")
    {
        // A host that stops reading its control channel in the middle of a worker's load: the worker holds
        // the channel, blocked in its send. The owner drops everything all the same, at once; the supervisor
        // kills the host on its own thread, which ends the worker's load.
        auto composition = HelperComposition { 0ms };
        auto timeouts = HostTimeouts {};
        timeouts.load = 30s;
        auto backend = composition.backend(
            helperOf(VTEXTENSION_HOSTILE_HELPER_PATH, "stop-reading-after-instantiate", timeouts));
        auto runtime = contract::createRuntime(*backend);
        auto module = [&] {
            auto loaded = runtime->load(ModuleIdentity { "same" }, toModule(contract::InstanceModule));
            REQUIRE(loaded.has_value());
            return *std::move(loaded);
        }();
        auto context = contract::RecordingHostContext {};
        auto instances = std::vector<std::unique_ptr<IWasmInstance>> {};
        for ([[maybe_unused]] auto const index: { 0, 1, 2 })
            instances.push_back(contract::instantiate(*module, context));

        auto loading = std::async(std::launch::async, [worker = runtime] {
            return worker->load(ModuleIdentity { "same" }, std::vector<std::byte>(LargeModuleBytes));
        });
        // Still blocked a while later: the host reads none of the module.
        REQUIRE(loading.wait_for(200ms) == std::future_status::timeout);

        auto dropping = std::async(std::launch::async, [&] {
            instances.clear();
            module.reset();
            runtime.reset();
            backend.reset();
        });
        within(dropping, DropBound, "dropping waited for the helper");

        auto const loaded = within(loading, Patience, "the worker's load did not end with its helper");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ExtensionError::HostUnavailable);
        CHECK(composition.awaitEveryHelperReaped());
        CHECK(composition.killsFor(KillReason::Dropped) == 1);
        CHECK(composition.supervisor().counters().reaped == 1);
        CHECK(composition.supervisor().counters().uncleanExits == 0);
        CHECK(everyKillRanOn(composition.recording(), composition.supervisor().threadId()));
    }

    SECTION("quitting")
    {
        // The supervisor goes with helpers that do not go by themselves: one that ignores its channels'
        // end, and (POSIX) one stopped, which cannot see it. It kills both and returns at once, and neither
        // is left once it has.
        auto composition = HelperComposition { 0ms };
        auto connections = std::vector<vtextension::wasm::remote::HostConnection> {};
        auto const connect = [&](HelperBackendConfig const& config) {
            auto connection = composition.connector(config)->connect();
            REQUIRE(connection.has_value());
            connections.push_back(*std::move(connection));
        };
        connect(helperOf(VTEXTENSION_HOSTILE_HELPER_PATH, "ignore-eof"));
#ifndef _WIN32
        connect(helperOf(VTEXTENSION_HELPER_PATH, ""));
#endif
        auto const processIds = composition.recording().processIds();
        REQUIRE(processIds.size() == connections.size());
#ifdef _WIN32
        auto const watches = std::array<ProcessWatch, 1> { ProcessWatch { processIds.front() } };
#else
        REQUIRE(::kill(static_cast<pid_t>(processIds.back()), SIGSTOP) == 0);
#endif
        // Their channels end; the handles are held on past the supervisor, as a hosting process's may be.
        for (auto& connection: connections)
        {
            auto const control = std::move(connection.control);
            auto const lanes = std::move(connection.lanes);
        }

        auto const supervisorThread = composition.supervisor().threadId();
        auto stopping = std::async(std::launch::async, [&] { composition.stopSupervisor(); });
        within(stopping, DropBound, "the supervisor waited for its helpers");
        CHECK(everyKillRanOn(composition.recording(), supervisorThread));
        CHECK(composition.recording().killThreads().size() == processIds.size());
#ifdef _WIN32
        for (auto const& watch: watches)
            CHECK(watch.endsWithin(OrphanBound));
#else
        for (auto const processId: processIds)
        {
            auto const isGone = childEndsWithin(processId, OrphanBound);
            CHECK(isGone);
            if (!isGone)
                killChild(processId);
        }
#endif
    }
}

TEST_CASE("no helper outlives its parent", "[vtextension][process]")
{
    // A hosting process killed outright -- no destructor, no supervisor stop -- takes its helper with it:
    // Linux and FreeBSD by the parent-death signal the helper bound to the supervisor's thread that started
    // it, Windows by the kill-on-close job. A helper that ignores its channels' end is the one that proves
    // it; macOS has only that end, so its benign helper goes and the other cannot be made to.
    becomeSubreaper();
    struct Row
    {
        char const* name;
        char const* helper;
        std::string_view hostile;
    };
    auto const rows = std::array {
        Row { .name = "benign", .helper = VTEXTENSION_HELPER_PATH, .hostile = "" },
        Row { .name = "ignore-eof", .helper = VTEXTENSION_HOSTILE_HELPER_PATH, .hostile = "ignore-eof" },
    };
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.name)
        {
#ifdef __APPLE__
            if (!row.hostile.empty())
                SKIP("macOS has no parent-death signal; documented");
#endif
            auto const launcher = process::makeSystemProcessLauncher();
            auto probe = process::testing::launchHelper(
                launcher,
                process::testing::HelperLaunch {
                    .executable = VTEXTENSION_PROBE_PATH,
                    .leading = { "--parent" },
                    .trailing =
                        [&](std::span<vtextension::wasm::remote::transport::NativeHandle const> /*handles*/) {
                            auto trailing = std::vector<std::string> { row.helper };
                            if (!row.hostile.empty())
                                trailing.push_back(std::format("--hostile={}", row.hostile));
                            return trailing;
                        },
                    .environment = process::testing::helperEnvironment() });
            REQUIRE(probe.has_value());
            auto reporting = std::async(std::launch::async, [&] { return helperReported(probe->control); });
            auto const helper = within(reporting, Patience, "the probe did not report its helper");
            REQUIRE(helper.has_value());
#ifdef _WIN32
            auto const watch = ProcessWatch { *helper };
#endif

            probe->process->kill();
            REQUIRE(process::testing::awaitReap(*launcher, *probe->process).has_value());
#ifdef _WIN32
            CHECK(watch.endsWithin(OrphanBound));
#elifdef __APPLE__
            auto const deadline = std::chrono::steady_clock::now() + OrphanBound;
            while (!(::kill(static_cast<pid_t>(*helper), 0) == -1 && errno == ESRCH)
                   && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            CHECK((::kill(static_cast<pid_t>(*helper), 0) == -1 && errno == ESRCH));
#else
            auto const isGone = childEndsWithin(*helper, OrphanBound);
            CHECK(isGone);
            if (!isGone)
                killChild(*helper);
#endif
        }
    }
}

TEST_CASE("teardown in coverage builds lets the helper exit", "[vtextension][process]")
{
    // With a teardown grace, a helper let go sees its channels end and exits on its own, writing what it has
    // to -- coverage data, sanitizer reports -- rather than being killed first.
    auto composition = HelperComposition { process::testing::HelperTeardownGrace };
    {
        auto const backend = composition.backend(helperOf(VTEXTENSION_HELPER_PATH, ""));
        auto const runtime = contract::createRuntime(*backend);
        auto const module = contract::load(*runtime, contract::InstanceModule);
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(*module, context);
    }
    REQUIRE(composition.awaitEveryHelperReaped());
    CHECK(composition.recording().exitStatuses()
          == std::vector { process::ExitStatus { .kind = process::ExitKind::Exited, .value = 0 } });
    CHECK(composition.killsFor(KillReason::Dropped) == 0);
    CHECK(composition.supervisor().counters().uncleanExits == 0);
}
