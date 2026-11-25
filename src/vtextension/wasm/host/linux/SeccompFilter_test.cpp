// SPDX-License-Identifier: Apache-2.0
//
// The extension host's system call filter, run by a BPF evaluator rather than installed: what each call is
// decided, on both architectures, and that no row set lets through what the sandbox exists to forbid.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sys/mman.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>

#include <vtextension/wasm/host/linux/SeccompFilter.hpp>
#include <vtextension/wasm/host/linux/SystemCallRows.hpp>
#include <vtextension/wasm/host/linux/testing/BpfEvaluator.hpp>

using namespace vtextension::wasm::host;
using vtextension::wasm::BuildInstrumentation;
using vtextension::wasm::host::testing::evaluateFilter;
using vtextension::wasm::host::testing::FilterVerdict;
using vtextension::wasm::host::testing::SystemCallUse;
using vtextension::wasm::host::testing::verdictOf;

namespace
{

constexpr auto Control = 7;
constexpr auto ALane = 8;
constexpr auto OwnProcess = 4242;
constexpr auto OtherProcess = 1;

constexpr auto Both = std::array { AuditArch::Amd64, AuditArch::AArch64 };

/// @return The number of @p call on @p arch, which must have it.
[[nodiscard]] long numberOf(SystemCall call, AuditArch arch)
{
    auto const number = systemCallNumber(call, arch);
    REQUIRE(number.has_value());
    return *number;
}

/// @return What the filter of @p instrumentation's rows on @p arch, for an engine running @p code, decides
/// for
///         @p call with @p arguments.
[[nodiscard]] FilterVerdict decide(BuildInstrumentation instrumentation,
                                   AuditArch arch,
                                   long number,
                                   std::array<uint64_t, 6> arguments = {},
                                   AuditArch madeOn = AuditArch::Amd64,
                                   GuestCode code = GuestCode::Compiled)
{
    auto const rows = rowsFor(code, instrumentation, arch);
    auto const program = assembleFilter(rows, SandboxDescriptors { .control = Control }, OwnProcess, arch);
    REQUIRE(program.has_value());
    auto const returned =
        evaluateFilter(*program, SystemCallUse { .arch = madeOn, .number = number, .arguments = arguments });
    REQUIRE(returned.has_value());
    return verdictOf(*returned);
}

constexpr auto Allowed = FilterVerdict { .action = SystemCallAction::Allow };
constexpr auto Killed = FilterVerdict { .action = SystemCallAction::Kill };

[[nodiscard]] constexpr FilterVerdict refusedWith(int error)
{
    return FilterVerdict { .action = SystemCallAction::Refuse, .error = error };
}

/// One call, and what the release rows decide for it.
struct Decision
{
    SystemCall call = SystemCall::Read;
    std::array<uint64_t, 6> arguments = {};
    FilterVerdict verdict = Killed;
    char const* what = "";
};

constexpr auto ReadWrite = uint64_t { PROT_READ | PROT_WRITE };
constexpr auto Anonymous = uint64_t { MAP_PRIVATE | MAP_ANONYMOUS };

} // namespace

TEST_CASE("SeccompFilter.decidesEachRowAsItsTableSays", "[vtextension][sandbox]")
{
    auto const decisions = std::array {
        Decision { SystemCall::Sendto, {}, Allowed, "send() on a channel" },
        Decision { SystemCall::Recvfrom, {}, Allowed, "recv() on a channel" },
        Decision { SystemCall::Futex, {}, Allowed, "a wait" },
        Decision { SystemCall::Recvmsg, { Control }, Allowed, "recvmsg() on control" },
        Decision { SystemCall::Recvmsg, { ALane }, Killed, "recvmsg() on a lane" },
        Decision {
            SystemCall::Recvmsg, { Control | (uint64_t { 1 } << 32U) }, Allowed, "an int's upper half" },
        Decision { SystemCall::Sendmsg, { Control }, Killed, "sendmsg(), which carries descriptors" },
        Decision { SystemCall::Sendmmsg, { Control }, Killed, "sendmmsg()" },
        Decision { SystemCall::Socket, { AF_INET }, refusedWith(EPERM), "an internet socket" },
        Decision { SystemCall::Socket, { AF_UNIX }, refusedWith(EPERM), "a unix socket" },
        Decision { SystemCall::Openat, {}, refusedWith(EACCES), "a file" },
        Decision { SystemCall::Ioctl, { 2 }, refusedWith(ENOTTY), "a terminal's question" },
        Decision { SystemCall::Mmap,
                   { 0, 4096, PROT_WRITE | PROT_EXEC, Anonymous },
                   Killed,
                   "writable and executable memory" },
        Decision {
            SystemCall::Mmap, { 0, 4096, PROT_READ | PROT_EXEC, Anonymous }, Killed, "executable memory" },
        Decision { SystemCall::Mmap, { 0, 4096, ReadWrite, Anonymous }, Allowed, "private memory" },
        Decision { SystemCall::Mmap, { 0, 4096, ReadWrite, MAP_SHARED }, Killed, "shared memory anywhere" },
        Decision { SystemCall::Mmap,
                   { 0, 4096, ReadWrite, MAP_SHARED_VALIDATE },
                   Killed,
                   "validated shared anywhere" },
        Decision { SystemCall::Mmap,
                   { 0x1000, 4096, ReadWrite, MAP_SHARED | MAP_FIXED, 9 },
                   Allowed,
                   "shared memory over a reservation" },
        Decision { SystemCall::Mprotect, { 0x1000, 4096, PROT_WRITE | PROT_EXEC }, Killed, "W+X" },
        Decision {
            SystemCall::Mprotect, { 0x1000, 4096, PROT_READ | PROT_EXEC }, Allowed, "code made executable" },
        Decision { SystemCall::Mprotect, { 0x1000, 4096, ReadWrite }, Allowed, "memory opened" },
        Decision { SystemCall::Clone, {}, Killed, "a thread or process" },
        Decision { SystemCall::Clone3, {}, Killed, "a thread or process" },
        Decision { SystemCall::Execve, {}, Killed, "a program" },
        Decision { SystemCall::Execveat, {}, Killed, "a program" },
        Decision { SystemCall::Ptrace, {}, Killed, "tracing" },
        Decision { SystemCall::Tgkill, { OwnProcess, 1, 6 }, Allowed, "abort()" },
        Decision { SystemCall::Tgkill, { OtherProcess, 1, 6 }, Killed, "another process's thread" },
        Decision { SystemCall::Kill, { OtherProcess, 0 }, Killed, "another process" },
        Decision { SystemCall::RtSigaction, {}, Allowed, "a fault handed on to the default action" },
        Decision { SystemCall::Writev, { 2 }, Allowed, "the C library's report of heap corruption" },
    };
    for (auto const arch: Both)
    {
        for (auto const& decision: decisions)
        {
            INFO((arch == AuditArch::Amd64 ? "x86_64: " : "aarch64: ") << decision.what);
            CHECK(
                decide(
                    BuildInstrumentation::None, arch, numberOf(decision.call, arch), decision.arguments, arch)
                == decision.verdict);
        }
        // What fork and vfork are, where they exist.
        for (auto const call: { SystemCall::Fork, SystemCall::Vfork })
            if (auto const number = systemCallNumber(call, arch))
                CHECK(decide(BuildInstrumentation::None, arch, *number, {}, arch) == Killed);
        // The coverage rows open the counter files; a sanitizer's, /proc.
        CHECK(decide(BuildInstrumentation::Coverage, arch, numberOf(SystemCall::Openat, arch), {}, arch)
              == Allowed);
        CHECK(decide(BuildInstrumentation::Sanitizers, arch, numberOf(SystemCall::Openat, arch), {}, arch)
              == Allowed);
        // ... and coverage maps them shared, never executable.
        auto const shared = std::array<uint64_t, 6> { 0, 4096, ReadWrite, MAP_SHARED };
        CHECK(decide(BuildInstrumentation::Coverage, arch, numberOf(SystemCall::Mmap, arch), shared, arch)
              == Allowed);
        auto const executable = std::array<uint64_t, 6> { 0, 4096, PROT_READ | PROT_EXEC, MAP_SHARED };
        CHECK(decide(BuildInstrumentation::Coverage, arch, numberOf(SystemCall::Mmap, arch), executable, arch)
              == Killed);
    }

    // The aarch64 instruction cache is flushed with membarrier; x86_64 needs none.
    CHECK(decide(BuildInstrumentation::None,
                 AuditArch::AArch64,
                 numberOf(SystemCall::Membarrier, AuditArch::AArch64),
                 {},
                 AuditArch::AArch64)
          == Allowed);
    CHECK(decide(BuildInstrumentation::None,
                 AuditArch::Amd64,
                 numberOf(SystemCall::Membarrier, AuditArch::Amd64))
          == Killed);

    // A foreign architecture, an x32 call and a number no row names.
    CHECK(decide(BuildInstrumentation::None,
                 AuditArch::Amd64,
                 numberOf(SystemCall::Read, AuditArch::Amd64),
                 {},
                 AuditArch::AArch64)
          == Killed);
    constexpr auto X32Bit = long { 0x4000'0000 };
    CHECK(decide(BuildInstrumentation::None,
                 AuditArch::Amd64,
                 X32Bit | numberOf(SystemCall::Read, AuditArch::Amd64))
          == Killed);
    CHECK(decide(BuildInstrumentation::None, AuditArch::Amd64, 4095) == Killed);
    CHECK(decide(BuildInstrumentation::None, AuditArch::AArch64, 4095, {}, AuditArch::AArch64) == Killed);
}

TEST_CASE("SeccompFilter.noRowSetAllowsTheForbidden", "[vtextension][sandbox]")
{
    constexpr auto Forbidden = std::array {
        SystemCall::Execve,
        SystemCall::Execveat,
        SystemCall::Ptrace,
        SystemCall::ProcessVmReadv,
        SystemCall::ProcessVmWritev,
        SystemCall::Socket,
        SystemCall::Connect,
        SystemCall::Bind,
        SystemCall::Sendmsg,
        SystemCall::Sendmmsg,
        SystemCall::Clone,
        SystemCall::Clone3,
        SystemCall::Fork,
        SystemCall::Vfork,
        SystemCall::Unshare,
        SystemCall::Setns,
        SystemCall::Mount,
        SystemCall::Bpf,
        SystemCall::PerfEventOpen,
        SystemCall::Accept,
        SystemCall::Accept4,
        SystemCall::Listen,
        SystemCall::Socketpair,
        SystemCall::Seccomp,
    };
    for (auto const arch: Both)
        for (auto const& rows: { releaseRows(arch),
                                 compiledRows(arch),
                                 interpretedRows(arch),
                                 sanitizerRows(arch),
                                 coverageRows(arch) })
            for (auto const call: Forbidden)
                if (auto const number = systemCallNumber(call, arch))
                    for (auto const& row: rows)
                    {
                        INFO(systemCallNumbers()[std::to_underlying(call)].name << ": " << row.reason);
                        CHECK_FALSE((row.number == *number && row.action == SystemCallAction::Allow));
                    }
}

TEST_CASE("SeccompFilter.everyRowSaysWhy", "[vtextension][sandbox]")
{
    for (auto const arch: Both)
        for (auto const& rows: { releaseRows(arch),
                                 compiledRows(arch),
                                 interpretedRows(arch),
                                 sanitizerRows(arch),
                                 coverageRows(arch) })
        {
            auto numbers = std::set<long> {};
            for (auto const& row: rows)
            {
                INFO("number " << row.number);
                CHECK_FALSE(row.reason.empty());
                CHECK(numbers.insert(row.number).second);
                CHECK((row.action == SystemCallAction::Refuse) == (row.error != 0));
            }
        }
}

TEST_CASE("SeccompFilter.assemblesForBothArchitectures", "[vtextension][sandbox]")
{
    // aarch64 has no open, fork or poll; its rows leave them out, and number the rest its own way.
    for (auto const call: { SystemCall::Open, SystemCall::Fork, SystemCall::Poll })
    {
        CHECK(systemCallNumber(call, AuditArch::Amd64).has_value());
        CHECK_FALSE(systemCallNumber(call, AuditArch::AArch64).has_value());
    }
    CHECK(systemCallNumber(SystemCall::Read, AuditArch::Amd64) == 0);
    CHECK(systemCallNumber(SystemCall::Read, AuditArch::AArch64) == 63);
    for (auto const arch: Both)
        for (auto const code: { GuestCode::Interpreted, GuestCode::Compiled })
            for (auto const instrumentation: { BuildInstrumentation::None,
                                               BuildInstrumentation::Coverage,
                                               BuildInstrumentation::Sanitizers })
            {
                auto const rows = rowsFor(code, instrumentation, arch);
                auto const program =
                    assembleFilter(rows, SandboxDescriptors { .control = Control }, OwnProcess, arch);
                REQUIRE(program.has_value());
                CHECK(program->size() <= BPF_MAXINSNS);
                // Every row is reached: each number decides as its row says, arguments aside.
                for (auto const& row: rows)
                    if (row.checks.empty())
                    {
                        auto const returned =
                            evaluateFilter(*program, SystemCallUse { .arch = arch, .number = row.number });
                        REQUIRE(returned.has_value());
                        CHECK(*returned == seccompReturnOf(row.action, row.error));
                    }
            }
    // A Refuse without an errno, or a check of a seventh argument, is no program.
    auto const noErrno = std::array { SystemCallRow {
        .number = 0, .checks = {}, .action = SystemCallAction::Refuse, .error = 0, .reason = "none" } };
    CHECK(assembleFilter(noErrno, SandboxDescriptors { .control = Control }, OwnProcess, AuditArch::Amd64)
              .error()
          == ConfinementFailure { .step = vtextension::wasm::remote::proto::LockdownStep::SystemCalls,
                                  .code = EINVAL });
    constexpr auto Seventh =
        std::array { ArgumentCheck { .index = 6, .test = ArgumentTest::Equals, .mask = 1, .value = 1 } };
    auto const seventh = std::array { SystemCallRow {
        .number = 0, .checks = Seventh, .action = SystemCallAction::Allow, .error = 0, .reason = "none" } };
    CHECK_FALSE(
        assembleFilter(seventh, SandboxDescriptors { .control = Control }, OwnProcess, AuditArch::Amd64)
            .has_value());
}

TEST_CASE("SeccompFilter.checksBothHalvesOfAWideArgument", "[vtextension][sandbox]")
{
    // A 64-bit test: the high half decides as much as the low one, both ways.
    constexpr auto Wide = uint64_t { 0x0000'0001'0000'0002 };
    constexpr auto EqualsWide = std::array { ArgumentCheck {
        .index = 1, .test = ArgumentTest::Equals, .mask = ~uint64_t { 0 }, .value = Wide } };
    constexpr auto DiffersFromWide = std::array { ArgumentCheck {
        .index = 1, .test = ArgumentTest::NotEquals, .mask = ~uint64_t { 0 }, .value = Wide } };
    for (auto const& [checks, whenEqual, whenNot]:
         { std::tuple { std::span<ArgumentCheck const> { EqualsWide }, Allowed, Killed },
           std::tuple { std::span<ArgumentCheck const> { DiffersFromWide }, Killed, Allowed } })
    {
        auto const rows = std::array { SystemCallRow { .number = 0,
                                                       .checks = checks,
                                                       .action = SystemCallAction::Allow,
                                                       .error = 0,
                                                       .reason = "wide" } };
        auto const program =
            assembleFilter(rows, SandboxDescriptors { .control = Control }, OwnProcess, AuditArch::Amd64);
        REQUIRE(program.has_value());
        for (auto const& [argument, expected]: { std::pair { Wide, whenEqual },
                                                 std::pair { Wide & 0xFFFF'FFFF, whenNot },
                                                 std::pair { Wide & ~uint64_t { 0xFFFF'FFFF }, whenNot },
                                                 std::pair { uint64_t { 0 }, whenNot } })
        {
            INFO(argument);
            auto const returned = evaluateFilter(
                *program,
                SystemCallUse { .arch = AuditArch::Amd64, .number = 0, .arguments = { 0, argument } });
            REQUIRE(returned.has_value());
            CHECK(verdictOf(*returned) == expected);
        }
    }
}

TEST_CASE("SeccompFilter.failedChecksTakeTheUnlistedAction", "[vtextension][sandbox]")
{
    // An outer sandbox refusing one value of an argument lets every other value through, as a call no row
    // names; the host's own filter, which kills what it does not name, kills a failed check as well.
    constexpr auto Refused = uint64_t { 83 };
    constexpr auto OneValue = std::array {
        ArgumentCheck { .index = 2, .test = ArgumentTest::Equals, .mask = 0xFFFF'FFFF, .value = Refused },
    };
    auto const rows = std::array { SystemCallRow { .number = 54,
                                                   .checks = OneValue,
                                                   .action = SystemCallAction::Refuse,
                                                   .error = EPERM,
                                                   .reason = "one option" } };
    for (auto const unlisted: { SystemCallAction::Allow, SystemCallAction::Kill })
    {
        auto const program = assembleFilter(
            rows, SandboxDescriptors { .control = Control }, OwnProcess, AuditArch::Amd64, unlisted);
        REQUIRE(program.has_value());
        auto const verdictFor = [&](long number, uint64_t option) {
            auto const returned = evaluateFilter(
                *program,
                SystemCallUse { .arch = AuditArch::Amd64, .number = number, .arguments = { 0, 0, option } });
            REQUIRE(returned.has_value());
            return verdictOf(*returned);
        };
        auto const otherwise = FilterVerdict { .action = unlisted };
        CHECK(verdictFor(54, Refused) == refusedWith(EPERM));
        CHECK(verdictFor(54, Refused + 1) == otherwise);
        CHECK(verdictFor(55, Refused) == otherwise);
    }
}

TEST_CASE("SeccompFilter.anInterpreterMakesNothingExecutable", "[vtextension][sandbox]")
{
    // An engine that compiles makes its code executable once written; one that interprets never does, and
    // asks only what the rows of its own allow.
    constexpr auto Executable = uint64_t { PROT_READ | PROT_EXEC };
    struct Expectation
    {
        SystemCall call {};
        std::array<uint64_t, 6> arguments {};
        FilterVerdict compiled;
        FilterVerdict interpreted;
        char const* what = nullptr;
    };
    auto const expectations = std::array {
        Expectation {
            SystemCall::Mprotect, { 0x1000, 4096, Executable }, Allowed, Killed, "code made executable" },
        Expectation { SystemCall::Mprotect, { 0x1000, 4096, PROT_WRITE | PROT_EXEC }, Killed, Killed, "W+X" },
        Expectation { SystemCall::Mprotect, { 0x1000, 4096, ReadWrite }, Allowed, Allowed, "memory opened" },
        Expectation {
            SystemCall::Mmap, { 0, 4096, Executable, Anonymous }, Killed, Killed, "executable memory" },
        Expectation { SystemCall::Fcntl, { 12, F_GETFL }, Killed, Allowed, "a descriptor's flags asked" },
        Expectation {
            SystemCall::Fcntl, { 12, F_SETFL, O_NONBLOCK }, Killed, Killed, "a descriptor's flags set" },
        Expectation { SystemCall::Fcntl, { 12, F_DUPFD, 0 }, Killed, Killed, "a descriptor duplicated" },
        Expectation { SystemCall::Lseek, { 12, 0, SEEK_SET }, Killed, Allowed, "a seek" },
        Expectation {
            SystemCall::SchedGetaffinity, { 0, 128 }, Killed, refusedWith(ENOSYS), "a thread's CPUs asked" },
    };
    for (auto const arch: Both)
        for (auto const& expectation: expectations)
        {
            INFO((arch == AuditArch::Amd64 ? "x86_64: " : "aarch64: ") << expectation.what);
            auto const number = numberOf(expectation.call, arch);
            CHECK(decide(BuildInstrumentation::None,
                         arch,
                         number,
                         expectation.arguments,
                         arch,
                         GuestCode::Compiled)
                  == expectation.compiled);
            CHECK(decide(BuildInstrumentation::None,
                         arch,
                         number,
                         expectation.arguments,
                         arch,
                         GuestCode::Interpreted)
                  == expectation.interpreted);
        }
    // aarch64 flushes instruction caches only for code it wrote.
    auto const membarrier = numberOf(SystemCall::Membarrier, AuditArch::AArch64);
    CHECK(decide(BuildInstrumentation::None,
                 AuditArch::AArch64,
                 membarrier,
                 {},
                 AuditArch::AArch64,
                 GuestCode::Interpreted)
          == Killed);
}
