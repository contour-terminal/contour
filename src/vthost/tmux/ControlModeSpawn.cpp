// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/ControlModeSpawn.hpp>

#include <vtpty/ProgramLookup.hpp>
#include <vtpty/SpawnTrampoline.hpp>

#ifndef _WIN32
    #include <vtpty/UnixUtils.hpp>
#endif

#include <core/net/Sockets.hpp>

#include <format>

#ifndef _WIN32
    #include <sys/wait.h>

    #include <algorithm>
    #include <csignal>
    #include <ranges>
    #include <tuple>
    #include <vector>

    #include <unistd.h>
#endif

namespace vthost::tmux
{

std::vector<std::string> controlModeArguments(std::string_view tmuxSocket)
{
    auto arguments = std::vector<std::string> { "tmux", "-C" };
    if (!tmuxSocket.empty())
    {
        arguments.emplace_back("-S");
        arguments.emplace_back(tmuxSocket);
    }
    arguments.emplace_back("attach-session");
    return arguments;
}

#ifndef _WIN32

std::expected<SpawnedControlMode, std::string> spawnControlMode(core::net::EventLoop& loop,
                                                                std::string const& tmuxSocket,
                                                                vtpty::ProgramSearch const& search,
                                                                vtpty::ProcessPlacement& placement)
{
    // The program's image and arguments are resolved here; the trampoline, single-threaded, does the rest of
    // the child-side setup that a forked child of this multi-threaded process could not do safely.
    auto const image = vtpty::findProgram("tmux", search);
    if (!image)
        return std::unexpected(std::format("tmux was not found on PATH ({})", search.path));

    // Parked on a gate, as a session's shell is, until the placement has moved it into a scope of its own.
    auto spawned = vtpty::spawnOnNewTerminal(
        vtpty::SpawnSpec {
            .image = *image,
            .argv = controlModeArguments(tmuxSocket),
            .lookup = vtpty::ExecLookup::ExactPath,
            .launch =
                vtpty::TerminalLaunch {
                    .controllingTerminal = vtpty::ControllingTerminal::Acquire,
                    .terminalSettings = vtpty::TerminalSettings::Keep,
                    .failureReport = vtpty::FailureReport::Silent,
                    .failureExit = vtpty::FailureExit::NotFound,
                },
        },
        placement,
        vtpty::ProcessRole::TmuxClient);
    if (!spawned)
        return std::unexpected(std::format("could not spawn the tmux client: {}", spawned.error().message()));
    auto const pid = spawned->pid;

    auto transport = core::net::adoptFd(loop, spawned->master.get());
    if (!transport)
    {
        spawned->master.reset();
        reapControlMode(pid);
        return std::unexpected("could not adopt the tmux pty: " + transport.error().toString());
    }
    std::ignore = spawned->master.release(); // the transport's now
    return SpawnedControlMode { .pid = pid, .transport = std::move(*transport) };
}
void reapControlMode(int pid)
{
    if (pid <= 0)
        return;
    // Up to five seconds for it to end on its own, looked at every tenth of a second.
    constexpr auto Looks = 50;
    constexpr auto Pause = useconds_t { 100'000 };
    auto status = 0;
    auto const ended = std::ranges::any_of(std::views::iota(0, Looks), [&](int /*look*/) {
        if (::waitpid(pid, &status, WNOHANG) != 0)
            return true;
        ::usleep(Pause);
        return false;
    });
    // Reaped, its pid may name another process by now: signalled only while it is still this one's child.
    if (ended)
        return;
    ::kill(pid, SIGKILL);
    // Waited for until reaped: a signal that interrupts the wait would leave it a zombie.
    std::ignore = vtpty::util::waitForExit(pid);
}

#else

std::expected<SpawnedControlMode, std::string> spawnControlMode(core::net::EventLoop& /*loop*/,
                                                                std::string const& /*tmuxSocket*/,
                                                                vtpty::ProgramSearch const& /*search*/,
                                                                vtpty::ProcessPlacement& /*placement*/)
{
    return std::unexpected("tmux control-mode spawning is not supported on Windows");
}

void reapControlMode(int /*pid*/)
{
}

#endif

} // namespace vthost::tmux
