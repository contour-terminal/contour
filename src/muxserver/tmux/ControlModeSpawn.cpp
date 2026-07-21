// SPDX-License-Identifier: Apache-2.0
#include <muxserver/tmux/ControlModeSpawn.h>

#include <format>

#include <net/Sockets.h>

#ifndef _WIN32
    #include <sys/wait.h>

    #include <csignal>
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

namespace muxserver::tmux
{

#ifndef _WIN32

std::expected<SpawnedControlMode, std::string> spawnControlMode(net::EventLoop& loop,
                                                                std::string const& tmuxSocket)
{
    auto master = -1;
    auto const pid = ::forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0)
        return std::unexpected(std::format("forkpty failed: errno {}", errno));
    if (pid == 0)
    {
        auto args = std::vector<std::string> { "tmux", "-C" };
        if (!tmuxSocket.empty())
        {
            args.emplace_back("-S");
            args.push_back(tmuxSocket);
        }
        args.emplace_back("attach-session");
        auto argv = std::vector<char*> {};
        argv.reserve(args.size() + 1);
        for (auto& arg: args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        ::execvp("tmux", argv.data());
        ::_exit(127);
    }

    auto transport = net::adoptFd(loop, master);
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
    auto status = 0;
    for (auto i = 0; i < 50 && ::waitpid(pid, &status, WNOHANG) == 0; ++i)
        ::usleep(100'000);
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, WNOHANG);
}

#else

std::expected<SpawnedControlMode, std::string> spawnControlMode(net::EventLoop& /*loop*/,
                                                                std::string const& /*tmuxSocket*/)
{
    return std::unexpected("tmux control-mode spawning is not supported on Windows");
}

void reapControlMode(int /*pid*/)
{
}

#endif

} // namespace muxserver::tmux
