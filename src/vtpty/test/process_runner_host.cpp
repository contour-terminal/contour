// SPDX-License-Identifier: Apache-2.0
//
// A process that dies holding a process runner (ProcessRunner_win32_test.cpp): `process_runner_host <probe>
// <pidfile>` starts `<probe> pid <pidfile> sleep 600000` on a PipedProcessRunner, pumps until the probe has
// written its pid, and then terminates itself with exit code 7 -- no destructor runs, so whatever ends the
// probe is what the system does with the runner's handles.
//
// Exit codes: 7 as above; 2 when the pid file did not appear within 10 s; 3 when the runner could not be
// made; 4 when the probe could not be started; 64 for a wrong command line.

#include <vtpty/ProcessRunner.hpp>

#include <core/WideString.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <Windows.h>
#include <shellapi.h>

using namespace std::chrono_literals;

namespace
{

/// How long the probe has to write its pid.
constexpr auto PidFileWait = 10s;

/// How long the probe may run: longer than it sleeps, so that no timeout ends it first.
constexpr auto ProbeTimeout = std::chrono::milliseconds { 3600s };

/// How long one round of pumping waits at most.
constexpr auto PumpSlice = 100ms;

/// The exit code it ends itself with, once the probe runs.
constexpr auto DiedHoldingTheRunner = UINT { 7 };

/// The exit codes of what went wrong before that.
enum class Failure : uint8_t
{
    PidFileMissing = 2,
    RunnerUnavailable = 3,
    ProbeNotStarted = 4,
    Usage = 64,
};

} // namespace

int main()
{
    auto count = 0;
    auto* const argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr || count != 3)
        return std::to_underlying(Failure::Usage);
    auto const arguments = std::span { argv, static_cast<std::size_t>(count) };
    auto const probe = std::filesystem::path { arguments[1] };
    auto const pidFile = std::filesystem::path { arguments[2] };
    LocalFree(static_cast<HLOCAL>(argv));

    auto runner = vtpty::PipedProcessRunner::create(core::platform::defaultSteadyClock(), {});
    if (!runner)
        return std::to_underlying(Failure::RunnerUnavailable);

    auto spec = vtpty::ProcessSpec {};
    spec.argv = { core::toUtf8(probe.wstring()), "pid", core::toUtf8(pidFile.wstring()), "sleep", "600000" };
    spec.environment = { { "PATH", core::toUtf8(probe.parent_path().wstring()) } };
    spec.timeout = ProbeTimeout;
    if (!(*runner)->start(std::move(spec), [](vtpty::ProcessToken, vtpty::ProcessExit const&) {}))
        return std::to_underlying(Failure::ProbeNotStarted);

    auto const deadline = std::chrono::steady_clock::now() + PidFileWait;
    auto const beforeDeadline = [&](auto) {
        return std::chrono::steady_clock::now() < deadline;
    };
    auto const written =
        std::ranges::any_of(std::views::iota(0) | std::views::take_while(beforeDeadline), [&](auto) {
            (*runner)->pump(PumpSlice);
            auto error = std::error_code {};
            return std::filesystem::exists(pidFile, error);
        });
    if (!written)
        return std::to_underlying(Failure::PidFileMissing);

    TerminateProcess(GetCurrentProcess(), DiedHoldingTheRunner);
    return static_cast<int>(DiedHoldingTheRunner);
}
