// SPDX-License-Identifier: Apache-2.0
//
// The Linux sandbox, for real: the probe locks itself down as the extension host does, in a process of its
// own, and tries one operation from a thread that ran before the lockdown. What is forbidden fails -- refused
// with an errno where a library may ask, the process killed by SIGSYS where nothing honest asks -- and what
// the host does still works, also inside an outer sandbox's filter.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sys/syscall.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/host/GuestCode.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

using namespace vtextension::wasm::remote;
using process::ExitKind;
using process::ExitStatus;
using process::testing::LaunchedHelper;
using process::testing::readReport;
using vtextension::wasm::host::GuestCode;

namespace
{

/// What a probe reported before its op ran, and what came of the op.
struct ProbeRun
{
    std::vector<std::string> lockdown;          ///< Its first report: the lockdown and the two opens.
    std::optional<std::vector<std::string>> op; ///< Its second: the op's outcome; nothing if it never came.
    std::optional<ExitStatus> exit;             ///< How it ended.
};

/// Whether a probe locks itself down as the host does.
enum class ProbeSandbox : uint8_t
{
    None,   ///< Only an outer filter confines it.
    System, ///< --sandboxed.
};

/// The name the probe's `--guest-code=` gives @p code.
[[nodiscard]] constexpr std::string_view guestCodeName(GuestCode code) noexcept
{
    switch (code)
    {
        case GuestCode::Interpreted: return "interpreted";
        case GuestCode::Compiled: return "compiled";
    }
    return "compiled";
}

/// @return The probe started sandboxed (or not), with the rows of an engine running @p guestCode, under
///         @p outerFilter, to run @p op.
[[nodiscard]] LaunchedHelper launchProbe(std::string_view op,
                                         std::string_view outerFilter,
                                         ProbeSandbox sandbox,
                                         GuestCode guestCode = GuestCode::Compiled)
{
    auto leading = std::vector<std::string> { "--receive-grant", std::format("--sandbox-op={}", op) };
    if (sandbox == ProbeSandbox::System)
    {
        leading.emplace_back("--sandboxed");
        leading.push_back(std::format("--guest-code={}", guestCodeName(guestCode)));
    }
    if (!outerFilter.empty())
        leading.push_back(std::format("--outer-filter={}", outerFilter));
    auto probe = process::testing::launchHelper(
        process::makeSystemProcessLauncher(),
        process::testing::HelperLaunch { .executable = VTEXTENSION_PROBE_PATH,
                                         .leading = std::move(leading),
                                         .environment = process::testing::sanitizerEnvironment() });
    REQUIRE(probe.has_value());
    return *std::move(probe);
}

/// Ends @p probe's connection, and reaps it.
[[nodiscard]] std::optional<ExitStatus> endProbe(LaunchedHelper& probe)
{
    probe.closeControl();
    auto status = process::testing::awaitReap(*probe.launcher, *probe.process);
    if (!status)
        CHECK(process::testing::endAndReap(*probe.launcher, *probe.process));
    return status;
}

/// Runs @p op in a probe, sandboxed unless said otherwise, under @p outerFilter, with the rows of an engine
/// running @p guestCode.
[[nodiscard]] ProbeRun runProbe(std::string_view op,
                                std::string_view outerFilter = {},
                                ProbeSandbox sandbox = ProbeSandbox::System,
                                GuestCode guestCode = GuestCode::Compiled)
{
    auto probe = launchProbe(op, outerFilter, sandbox, guestCode);
    auto run = ProbeRun {};
    auto const lockdown = readReport(probe.lanes.front());
    REQUIRE(lockdown.has_value());
    run.lockdown = *lockdown;
    run.op = readReport(probe.lanes.at(1));
    run.exit = endProbe(probe);
    return run;
}

/// @return The value of @p key in @p report.
[[nodiscard]] std::string valueOf(std::vector<std::string> const& report, std::string_view key)
{
    auto const prefix = std::format("{}=", key);
    auto const line =
        std::ranges::find_if(report, [&](std::string const& text) { return text.starts_with(prefix); });
    REQUIRE(line != report.end());
    return line->substr(prefix.size());
}

/// @return The Landlock ABI this kernel has; 0 if none.
[[nodiscard]] long landlockAbi()
{
    constexpr auto Version = 1U;
    auto const version = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, Version);
    return version < 0 ? 0 : version;
}

constexpr auto Killed = ExitStatus { .kind = ExitKind::Signalled, .value = SIGSYS };
constexpr auto Clean = ExitStatus { .kind = ExitKind::Exited, .value = 0 };
constexpr auto FirstAbiWithThreadSync = 8;

/// The refusal an op's report says.
[[nodiscard]] std::string refused(int error)
{
    return std::format("refused:{}", error);
}

} // namespace

TEST_CASE("Sandbox.forbiddenOperationsFailFromAnExecutor", "[vtextension][sandbox]")
{
    struct Expectation
    {
        std::string_view op;
        std::optional<int> refusedWith; ///< Nothing: the process is killed.
        /// The instrumented build whose rows allow it after all: a coverage build maps its counter files
        /// shared.
        std::optional<vtextension::wasm::BuildInstrumentation> allowedIn = std::nullopt;
    };
    constexpr auto Expectations = std::array {
        Expectation { .op = "open-file", .refusedWith = EACCES },
        Expectation { .op = "open-directory", .refusedWith = EACCES },
        // A sanitizer build reads its own maps for a report, and no other process's.
        Expectation { .op = "open-own-process",
                      .refusedWith = EACCES,
                      .allowedIn = vtextension::wasm::BuildInstrumentation::Sanitizers },
        Expectation { .op = "open-other-process", .refusedWith = EACCES },
        Expectation { .op = "socket-inet", .refusedWith = EPERM },
        Expectation { .op = "socket-unix", .refusedWith = EPERM },
        Expectation { .op = "send-descriptor", .refusedWith = std::nullopt },
        Expectation { .op = "fork", .refusedWith = std::nullopt },
        Expectation { .op = "spawn-thread", .refusedWith = std::nullopt },
        Expectation { .op = "execve", .refusedWith = std::nullopt },
        Expectation { .op = "ptrace-traceme", .refusedWith = std::nullopt },
        Expectation { .op = "signal-parent", .refusedWith = std::nullopt },
        Expectation { .op = "map-writable-executable", .refusedWith = std::nullopt },
        Expectation { .op = "map-shared-unfixed",
                      .refusedWith = std::nullopt,
                      .allowedIn = vtextension::wasm::BuildInstrumentation::Coverage },
        Expectation { .op = "protect-writable-executable", .refusedWith = std::nullopt },
    };
    for (auto const& expectation: Expectations)
    {
        INFO(expectation.op);
        auto const run = runProbe(expectation.op);
        CHECK(valueOf(run.lockdown, "lockdown") == "locked");
        if (expectation.allowedIn == vtextension::wasm::ThisBuildInstrumentation)
        {
            REQUIRE(run.op.has_value());
            CHECK(valueOf(*run.op, "op") == "ok");
            CHECK(run.exit == Clean);
        }
        else if (expectation.refusedWith)
        {
            REQUIRE(run.op.has_value());
            CHECK(valueOf(*run.op, "op") == refused(*expectation.refusedWith));
            CHECK(run.exit == Clean);
        }
        else
        {
            // Killed before it could say anything: its report never came, and its status names the filter.
            CHECK_FALSE(run.op.has_value());
            CHECK(run.exit == Killed);
        }
    }
}

TEST_CASE("Sandbox.onlyCompiledCodeTurnsExecutable", "[vtextension][sandbox]")
{
    struct Expectation
    {
        GuestCode guestCode;
        std::string_view op;
        ExitStatus exit; ///< Killed, or Clean once the op worked.
    };
    // An interpreter writes no code: nothing of its host's turns executable, by mapping or protecting --
    // while memory it protects without the execute bit, as guest memory opens, is protected as asked. An
    // engine that compiles turns what it wrote read-and-execute; it never maps memory executable.
    constexpr auto Expectations = std::array {
        Expectation { .guestCode = GuestCode::Interpreted, .op = "protect-readable", .exit = Clean },
        Expectation { .guestCode = GuestCode::Interpreted, .op = "protect-executable", .exit = Killed },
        Expectation { .guestCode = GuestCode::Interpreted, .op = "map-executable", .exit = Killed },
        Expectation { .guestCode = GuestCode::Compiled, .op = "protect-executable", .exit = Clean },
        Expectation { .guestCode = GuestCode::Compiled, .op = "map-executable", .exit = Killed },
    };
    for (auto const& expectation: Expectations)
    {
        INFO(guestCodeName(expectation.guestCode) << ": " << expectation.op);
        auto const run = runProbe(expectation.op, {}, ProbeSandbox::System, expectation.guestCode);
        CHECK(valueOf(run.lockdown, "lockdown") == "locked");
        CHECK(run.exit == expectation.exit);
        if (expectation.exit == Clean)
        {
            REQUIRE(run.op.has_value());
            CHECK(valueOf(*run.op, "op") == "ok");
        }
        else
            CHECK_FALSE(run.op.has_value());
    }
}

TEST_CASE("Sandbox.permittedOperationsWorkFromAnExecutor", "[vtextension][sandbox]")
{
    for (auto const outerFilter: { std::string_view {}, std::string_view { "flatpak" } })
    {
        INFO("outer filter: " << outerFilter);
        auto probe = launchProbe("permitted", outerFilter, ProbeSandbox::System);
        auto const lockdown = readReport(probe.lanes.front());
        REQUIRE(lockdown.has_value());
        CHECK(valueOf(*lockdown, "lockdown") == "locked");

        // Granted an object after it locked down, it takes it in and maps it as the host does.
        auto control = transport::PduStream { std::move(probe.control),
                                              proto::ChannelKind::Control,
                                              proto::Direction::ToParent,
                                              proto::PduLimits {} };
        constexpr auto ObjectBytes = proto::memoryObjectBytes(0);
        auto const object =
            transport::createSharedObject(ObjectBytes, transport::SharedObjectUse::GuestMemory);
        REQUIRE(object.has_value());
        auto mapping =
            transport::SharedMapping::map(object->get(), ObjectBytes, transport::MapAccess::ReadWrite);
        REQUIRE(mapping.has_value());
        REQUIRE(mapping->commit(0, ObjectBytes).has_value());
        auto const target = probe.process->handleTarget();
        REQUIRE(target.has_value());
        auto const grant = transport::grantHandle(*target, object->get());
        REQUIRE(grant.has_value());
        auto const instantiate = proto::Instantiate {
            .moduleId = 1,
            .instanceId = 1,
            .interruptSlot = 0,
            .memory = proto::MemoryGrant { .handle = grant->wireValue,
                                           .objectBytes = ObjectBytes,
                                           .initialBytes = ObjectBytes },
        };
        REQUIRE(control.send(instantiate, *grant).has_value());

        auto const op = readReport(probe.lanes.at(1));
        REQUIRE(op.has_value());
        CHECK(valueOf(*op, "op") == "ok");
        constexpr auto Mark = std::string_view { "probe" };
        CHECK(std::ranges::equal(mapping->bytes().first(Mark.size()), std::as_bytes(std::span { Mark })));

        control.shutdown();
        auto const status = process::testing::awaitReap(*probe.launcher, *probe.process);
        CHECK(status == Clean);
        if (!status)
            CHECK(process::testing::endAndReap(*probe.launcher, *probe.process));
    }
}

TEST_CASE("Sandbox.landlockCoversEveryThread", "[vtextension][sandbox]")
{
    auto probe = launchProbe("none", {}, ProbeSandbox::System);
    auto const lockdown = readReport(probe.lanes.front());
    REQUIRE(lockdown.has_value());
    CHECK(valueOf(*lockdown, "lockdown") == "locked");

    // Every thread runs the filter, and gains no privilege: the main thread, the one started before the
    // restriction and the one after.
    auto const tasks = std::filesystem::path { std::format("/proc/{}/task", probe.process->id()) };
    auto threads = 0;
    for (auto const& task: std::filesystem::directory_iterator { tasks })
    {
        auto status = std::ifstream { task.path() / "status" };
        auto lines = std::vector<std::string> {};
        for (auto line = std::string {}; std::getline(status, line);)
            lines.push_back(line);
        INFO(task.path().string());
        CHECK(std::ranges::contains(lines, std::string { "Seccomp:\t2" }));
        CHECK(std::ranges::contains(lines, std::string { "NoNewPrivs:\t1" }));
        threads += 1;
    }
    CHECK(threads >= 3);

    // Opening a file between the restriction and the lockdown -- Landlock's, not the filter's -- fails from
    // a thread started after the restriction, and from one started before where the ABI reaches it (TSYNC).
    auto const abi = landlockAbi();
    if (abi == 0)
        SKIP("this kernel has no Landlock");
    CHECK(valueOf(*lockdown, "after-restrict") == refused(EACCES));
    if (abi >= FirstAbiWithThreadSync)
        CHECK(valueOf(*lockdown, "before-restrict") == refused(EACCES));
    else
        CHECK(valueOf(*lockdown, "before-restrict") == "ok");
    CHECK(endProbe(probe) == Clean);
}

TEST_CASE("Sandbox.locksDownWithoutLandlock", "[vtextension][sandbox]")
{
    // Inside a filter that has no Landlock, the host locks down all the same, by its filter alone.
    auto const run = runProbe("open-file", "no-landlock");
    CHECK(valueOf(run.lockdown, "lockdown") == "locked");
    CHECK(valueOf(run.lockdown, "after-restrict") == "ok");
    REQUIRE(run.op.has_value());
    // The filter refuses files; an instrumented build's lets them through for its reports and counters, and
    // leaves them to Landlock, which is missing here.
    auto const expected =
        vtextension::wasm::ThisBuildInstrumentation == vtextension::wasm::BuildInstrumentation::None
            ? refused(EACCES)
            : std::string { "ok" };
    CHECK(valueOf(*run.op, "op") == expected);
    CHECK(run.exit == Clean);
}

TEST_CASE("Sandbox.locksDownUnderAFlatpakLikeFilter", "[vtextension][sandbox]")
{
    auto const run = runProbe("open-file", "flatpak");
    CHECK(valueOf(run.lockdown, "lockdown") == "locked");
    REQUIRE(run.op.has_value());
    CHECK(valueOf(*run.op, "op") == refused(EACCES));
    CHECK(run.exit == Clean);
    // Its permitted operations: Sandbox.permittedOperationsWorkFromAnExecutor, under the same filter.
}

TEST_CASE("Sandbox.threadSyncFailureFailsLockdown", "[vtextension][sandbox]")
{
    // A thread that installed a filter of its own cannot take the host's: the lockdown fails, and says so.
    auto const run = runProbe("none", "thread");
    CHECK(valueOf(run.lockdown, "lockdown")
          == std::format("failed:{}:{}", std::to_underlying(proto::LockdownStep::ThreadSync), EPERM));
    CHECK(run.exit == Clean);
}

TEST_CASE("Channel.refusingDescriptorsMayBeUnavailable", "[vtextension][sandbox]")
{
    // Where an outer filter refuses SO_PASSRIGHTS, Contour's end discards what a helper attaches instead.
    auto const run = runProbe("channel-pair", "passrights", ProbeSandbox::None);
    CHECK(valueOf(run.lockdown, "lockdown") == "none");
    REQUIRE(run.op.has_value());
    CHECK(valueOf(*run.op, "op") == "upward:discarded");
    CHECK(run.exit == Clean);
}
