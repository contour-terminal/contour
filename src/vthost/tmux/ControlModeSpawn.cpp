// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/ControlModeSpawn.hpp>

#include <vtpty/ProgramLookup.hpp>

#include <core/net/Sockets.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <format>

#ifndef _WIN32
    #include <sys/wait.h>

    #include <algorithm>
    #include <cerrno>
    #include <csignal>
    #include <ranges>
    #include <vector>

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
                                                                std::string_view searchPath)
{
    // Everything the child uses is made here. Between forkpty() and exec() a child of this multi-threaded
    // process may only call async-signal-safe functions: no allocation, and no PATH walk, which execvp()
    // does.
    auto const image =
        vtpty::resolveExecutablePath("tmux", searchPath, core::platform::NativeFileSystem::instance());
    if (!image.contains('/'))
        return std::unexpected(std::format("tmux was not found on PATH ({})", searchPath));
    auto arguments = controlModeArguments(tmuxSocket);
    auto argv = std::vector<char*> {};
    argv.reserve(arguments.size() + 1);
    for (auto& argument: arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    auto master = -1;
    auto const pid = ::forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0)
        return std::unexpected(std::format("forkpty failed: errno {}", errno));
    if (pid == 0)
    {
        ::execv(image.c_str(), argv.data());
        ::_exit(127);
    }

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
                                                                std::string_view /*searchPath*/)
{
    return std::unexpected("tmux control-mode spawning is not supported on Windows");
}

void reapControlMode(int /*pid*/)
{
}

#endif

} // namespace vthost::tmux
