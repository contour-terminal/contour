// SPDX-License-Identifier: Apache-2.0
/// @file
/// The process session-oom-isolation.py runs as a systemd unit with OOMPolicy=stop.
///
///   session_oom_driver --placement=systemd|none   spawns `--hog` through vtpty::Process, and
///                                                 reports how it ended
///   session_oom_driver --hog                      prints its cgroup, then allocates until killed

#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/SdBusScopeBus.hpp>
#include <vtpty/SystemdScopePlacement.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

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

/// Prints this process's cgroup, then touches memory until the kernel kills it.
/// @return ExitLimitNotEnforced, should it reach the ceiling alive: no memory limit applies here, and
///         going on would exhaust the machine instead of the cgroup.
int hog()
{
    // Streamed: /proc reports its files as empty, so a read sized by the file's size gets nothing.
    std::print("{}", (std::ostringstream {} << std::ifstream { "/proc/self/cgroup" }.rdbuf()).str());
    std::fflush(stdout);
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
        vtpty::Process::ExecInfo { .program = std::filesystem::read_symlink("/proc/self/exe").string(),
                                   .arguments = { "--hog" },
                                   .workingDirectory = {},
                                   .env = {} },
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

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    if (arguments.size() != 2)
        return ExitUsage;
    auto const argument = std::string_view { arguments[1] };
    if (argument == "--hog")
        return hog();
    if (argument.starts_with("--placement="))
        return drive(argument.substr(std::string_view { "--placement=" }.size()));
    return ExitUsage;
}
