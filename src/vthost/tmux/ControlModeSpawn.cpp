// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/ControlModeSpawn.hpp>

#include <vtpty/ProgramLookup.hpp>
#include <vtpty/SpawnTrampoline.hpp>

#include <core/net/Sockets.hpp>

#include <format>

#ifndef _WIN32
    #include <sys/wait.h>

    #include <algorithm>
    #include <array>
    #include <cerrno>
    #include <csignal>
    #include <ranges>
    #include <vector>

    #include <fcntl.h>

    #ifdef __APPLE__
        #include <util.h>
    #elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
        #include <libutil.h>
    #else
        #include <pty.h>
    #endif
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
    auto gate = vtpty::makeGate();
    if (!gate)
        return std::unexpected(
            std::format("could not make the tmux client's gate: {}", gate.error().message()));

    auto master = -1;
    auto slave = -1;
    if (::openpty(&master, &slave, nullptr, nullptr, nullptr) < 0)
        return std::unexpected(std::format("openpty failed: errno {}", errno));
    for (auto const fd: { master, slave })
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    auto spec = vtpty::SpawnSpec {};
    spec.mode = vtpty::SpawnMode::Terminal;
    spec.image = *image;
    spec.argv = controlModeArguments(tmuxSocket);
    spec.lookup = vtpty::ExecLookup::ExactPath;
    spec.controllingTerminal = vtpty::ControllingTerminal::Acquire;
    spec.terminalSettings = vtpty::TerminalSettings::Keep;
    spec.failureReport = vtpty::FailureReport::Silent;
    auto const fds = std::array {
        vtpty::InheritedFd { vtpty::TrampolineSlot::Gate, gate->childEnd.get() },
        vtpty::InheritedFd { vtpty::TrampolineSlot::Terminal, slave },
    };
    auto const spawned = vtpty::spawnTrampoline(std::move(spec), fds);
    ::close(slave);
    if (!spawned)
    {
        ::close(master);
        return std::unexpected(std::format("could not spawn the tmux client: {}", spawned.error().message()));
    }
    auto const pid = *spawned;
    gate->childEnd.reset();
    placement.placeThenRelease(vtpty::ParkedChild {
        pid, vtpty::openPidFd(pid), std::move(gate->parentEnd), vtpty::ProcessRole::TmuxClient });

    auto transport = core::net::adoptFd(loop, master);
    if (!transport)
    {
        ::close(master);
        reapControlMode(pid);
        return std::unexpected("could not adopt the tmux pty: " + transport.error().toString());
    }
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
    while (::waitpid(pid, &status, 0) == -1 && errno == EINTR)
        ;
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
