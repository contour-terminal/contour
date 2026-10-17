// SPDX-License-Identifier: Apache-2.0
/// @file
/// The process session-oom-isolation.py runs as a systemd unit with OOMPolicy=stop.
///
///   session_oom_driver --placement=systemd|none   spawns `--hog` through vtpty::Process, and
///                                                 reports how it ended
///   session_oom_driver --hog                      prints its cgroup, then allocates until killed
///   session_oom_driver --report-placements=<unit prefix>
///                                                 places `--idle` children through fresh
///                                                 placements, and checks that systemd's answer to
///                                                 each request reached the placement
///   session_oom_driver --idle                     prints its cgroup, then waits to be killed

#include <vtpty/Process.hpp>
#include <vtpty/ProcessCgroup.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/SdBusScopeBus.hpp>
#include <vtpty/SystemdScopePlacement.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <core/platform/Clock.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

using namespace std::chrono_literals;

namespace
{

constexpr auto ChunkBytes = std::size_t { 1 } << 20;
constexpr auto HogCeilingBytes = std::size_t { 1 } << 30;
constexpr auto ScopeMemoryMax = std::uint64_t { 128 } << 20;

constexpr auto ExitFailed = 1;
constexpr auto ExitUsage = 2;
constexpr auto ExitLimitNotEnforced = 3;

/// Longer than systemd takes to act on OOMPolicy=stop once a process in the unit was killed.
constexpr auto GracePeriod = 5s;

/// How many fresh placements --report-placements makes, and how many children each places: the first
/// request of a connection, and a later one.
constexpr auto PlacementRounds = 5;
constexpr auto RequestsPerPlacement = 2;

/// Prints this process's cgroup, as /proc tells it.
void printCgroup()
{
    std::print("{}", vtpty::readProcessCgroup(::getpid()).value_or(""));
    std::fflush(stdout);
}

/// Prints this process's cgroup, then waits to be killed.
int idle()
{
    printCgroup();
    ::pause();
    return 0;
}

/// Prints this process's cgroup, then touches memory until the kernel kills it.
/// @return ExitLimitNotEnforced, should it reach the ceiling alive: no memory limit applies here, and
///         going on would exhaust the machine instead of the cgroup.
int hog()
{
    printCgroup();
    auto chunks = std::vector<std::vector<char>> {};
    for ([[maybe_unused]] auto const chunk: std::views::iota(std::size_t { 0 }, HogCeilingBytes / ChunkBytes))
        chunks.emplace_back(ChunkBytes, char { 0x5a });
    return ExitLimitNotEnforced;
}

/// @return The placement named @p name, or null when there is none by that name.
std::shared_ptr<vtpty::ProcessPlacement> placementNamed(std::string_view name)
{
    if (name == "none")
        return std::make_shared<vtpty::NoPlacement>();
    if (name != "systemd")
        return nullptr;
    auto config = vtpty::SystemdScopeConfig {};
    config.memoryLimit = vtpty::MemoryLimit { .maxBytes = ScopeMemoryMax, .swapMaxBytes = 0 };
    return vtpty::makeSystemdScopePlacement(std::move(config));
}

/// @return What @p process wrote, up to the end of its first line naming a cgroup (at most ~30 s).
std::string readCgroupLine(vtpty::Process& process)
{
    return vtpty::testing::readUntil(process, [](std::string_view text) {
        auto const at = text.find("0::");
        return at != std::string_view::npos && text.find('\n', at) != std::string_view::npos;
    });
}

/// Spawns the hog under @p placementName and checks how it ended.
/// @return 0 when the hog was killed and, with systemd placement, ran in a session scope.
int drive(std::string_view placementName)
{
    auto placement = placementNamed(placementName);
    if (!placement)
        return ExitUsage;

    auto process = vtpty::Process {
        vtpty::testing::execInfo(std::filesystem::read_symlink("/proc/self/exe").string(), { "--hog" }),
        vtpty::createPty(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }, std::nullopt),
        /*escapeSandbox=*/false,
        placement
    };
    if (auto const started = process.start(); !started)
    {
        std::println(stderr, "cannot start the hog: {}", started.error());
        return ExitFailed;
    }

    auto const cgroup = readCgroupLine(process);
    std::println("hog cgroup: {}", cgroup);
    auto const status = process.wait();
    if (!status)
        return ExitFailed;
    if (auto const* const exited = std::get_if<vtpty::Process::NormalExit>(&*status))
    {
        std::println(stderr, "the hog exited with {} instead of being killed", exited->exitCode);
        return exited->exitCode == ExitLimitNotEnforced ? ExitLimitNotEnforced : ExitFailed;
    }
    if (auto const signum = std::get<vtpty::Process::SignalExit>(*status).signum; signum != SIGKILL)
    {
        std::println(stderr, "the hog died by signal {}, not SIGKILL", signum);
        return ExitFailed;
    }
    if (placementName == "systemd" && !cgroup.contains("/contour-session-"))
    {
        std::println(stderr, "the hog did not run in a session scope");
        return ExitFailed;
    }

    // Stay alive long enough for systemd to have stopped this unit, were it going to.
    std::this_thread::sleep_for(GracePeriod);
    std::println("survived the hog's out-of-memory kill");
    return 0;
}

/// What systemd answered one scope request.
struct Answer
{
    std::string unitName;
    std::expected<void, vtpty::ScopeError> outcome;
};

/// The answers a placement's bus gave, request by request. Shared with the bus, which the placement
/// owns and asks on its worker.
class AnswerLog
{
  public:
    void add(Answer answer)
    {
        auto const _ = std::lock_guard { _mutex };
        _answers.push_back(std::move(answer));
    }

    /// @return Every answer so far, in request order.
    [[nodiscard]] std::vector<Answer> answers()
    {
        auto const _ = std::lock_guard { _mutex };
        return _answers;
    }

  private:
    std::mutex _mutex;
    std::vector<Answer> _answers;
};

/// The real bus, noting each answer it gives the placement.
class RecordingScopeBus final: public vtpty::ScopeBus
{
  public:
    RecordingScopeBus(std::unique_ptr<vtpty::ScopeBus> bus, std::shared_ptr<AnswerLog> log):
        _bus { std::move(bus) }, _log { std::move(log) }
    {
    }

    [[nodiscard]] std::expected<void, vtpty::ScopeError> startScope(
        vtpty::ScopeRequest const& request, std::chrono::milliseconds deadline) override
    {
        auto outcome = _bus->startScope(request, deadline);
        _log->add(Answer { .unitName = request.unitName, .outcome = outcome });
        return outcome;
    }

  private:
    std::unique_ptr<vtpty::ScopeBus> _bus;
    std::shared_ptr<AnswerLog> _log;
};

/// Places a first and a later `--idle` child through each of several fresh placements, and checks that
/// systemd's answer to every request reached the placement -- not only that the child ran in its scope,
/// which systemd sees to whether or not its answer is heard.
/// @param unitPrefix What every scope's name starts with, so that the caller can find and stop them.
/// @return 0 when every request was answered as placed, and each child ran in the scope it asked for.
int reportPlacements(std::string_view unitPrefix)
{
    auto& clock = core::platform::defaultSteadyClock();
    auto const self = std::filesystem::read_symlink("/proc/self/exe").string();
    auto failures = 0;
    for (auto const round: std::views::iota(1, PlacementRounds + 1))
    {
        auto const log = std::make_shared<AnswerLog>();
        auto config = vtpty::SystemdScopeConfig {};
        config.unitPrefix = unitPrefix;
        auto const placement = std::make_shared<vtpty::SystemdScopePlacement>(
            std::move(config),
            [&clock, log]() -> std::expected<std::unique_ptr<vtpty::ScopeBus>, vtpty::ScopeError> {
                auto bus = vtpty::SdBusScopeBus::connect(clock);
                if (!bus)
                    return std::unexpected(bus.error());
                return std::make_unique<RecordingScopeBus>(std::move(*bus), log);
            },
            vtpty::readProcessCgroup,
            clock);
        for (auto const request: std::views::iota(1, RequestsPerPlacement + 1))
        {
            auto process = vtpty::Process { vtpty::testing::execInfo(self, { "--idle" }),
                                            vtpty::createPty(vtpty::PageSize { vtpty::LineCount(24),
                                                                               vtpty::ColumnCount(80) },
                                                             std::nullopt),
                                            /*escapeSandbox=*/false,
                                            placement };
            if (auto const started = process.start(); !started)
            {
                std::println(stderr, "cannot start an idle child: {}", started.error());
                return ExitFailed;
            }
            // The child prints once released, which the placement does only after the bus answered --
            // if it asked at all.
            auto const cgroup = readCgroupLine(process);
            process.terminate(vtpty::Process::TerminationHint::Kill);
            (void) process.wait();

            auto const label = std::format("round {} request {}", round, request);
            auto const answers = log->answers();
            if (answers.size() < static_cast<std::size_t>(request))
            {
                std::println(stderr, "{}: released without asking systemd", label);
                ++failures;
                continue;
            }
            auto const& answer = answers[static_cast<std::size_t>(request) - 1];
            if (!answer.outcome)
            {
                std::println(stderr,
                             "{}: {} not reported placed: {}",
                             label,
                             answer.unitName,
                             vtpty::traitsOf(answer.outcome.error()).description);
                ++failures;
            }
            else if (!cgroup.contains("/" + answer.unitName))
            {
                std::println(
                    stderr, "{}: reported placed in {}, but ran in {}", label, answer.unitName, cgroup);
                ++failures;
            }
            else
                std::println("{}: placed in {}", label, answer.unitName);
        }
    }
    return failures == 0 ? 0 : ExitFailed;
}

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    if (arguments.size() != 2)
        return ExitUsage;
    auto const argument = std::string_view { arguments[1] };
    if (argument == "--hog")
        return hog();
    if (argument == "--idle")
        return idle();
    if (argument.starts_with("--placement="))
        return drive(argument.substr(std::string_view { "--placement=" }.size()));
    if (argument.starts_with("--report-placements="))
        return reportPlacements(argument.substr(std::string_view { "--report-placements=" }.size()));
    return ExitUsage;
}
