// SPDX-License-Identifier: Apache-2.0
#include <vthost/StdioChild.hpp>

#ifndef _WIN32

    #include <vtpty/OwnedFd.hpp>

    #include <core/net/Sockets.hpp>

    #include <sys/socket.h>
    #include <sys/wait.h>

    #include <array>
    #include <cerrno>
    #include <csignal>
    #include <format>
    #include <initializer_list>
    #include <system_error>
    #include <thread>
    #include <utility>
    #include <vector>

    #include <fcntl.h>
    #include <spawn.h>
    #include <unistd.h>

    #include <vthost/StderrTail.hpp>

// POSIX requires no header to declare `environ`; src/vtpty/Process_unix.cpp explains the per-platform story.
    #ifdef __APPLE__
        #include <crt_externs.h>
        #define environ (*_NSGetEnviron())
    #elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" char** environ;
    #endif

namespace vthost
{

namespace
{
    /// How long terminate() gives a child between SIGTERM and SIGKILL.
    constexpr auto TerminateGrace = std::chrono::seconds { 1 };

    [[nodiscard]] StdioSpawnError systemError(std::string_view what, int error)
    {
        return StdioSpawnError { .kind = SpawnFailure::SystemError,
                                 .message =
                                     std::format("{}: {}", what, std::system_category().message(error)) };
    }

    #ifdef SOCK_CLOEXEC
    /// Creates the socket pair close-on-exec at once, where the platform can.
    constexpr auto SocketPairFlags = SOCK_CLOEXEC;
    #else
    constexpr auto SocketPairFlags = 0;
    #endif

    /// Creates a pipe, close-on-exec at once where the platform can (pipe2), else just after.
    /// @return 0, or the error number.
    [[nodiscard]] int makePipe(std::array<int, 2>& fds) noexcept
    {
    #if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        return ::pipe2(fds.data(), O_CLOEXEC) < 0 ? errno : 0;
    #else
        return ::pipe(fds.data()) < 0 ? errno : 0;
    #endif
    }

    /// Marks @p fd close-on-exec: on macOS, whose socketpair() and pipe() cannot as they create it, a
    /// thread forking in between would leak it into that child; everywhere else it is set already.
    /// @return 0, or the error number.
    [[nodiscard]] int setCloseOnExec(int fd) noexcept
    {
        return ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? errno : 0;
    }

    /// Copies @p fd into @p tail until it ends, then closes it. Detached: a grandchild may hold the
    /// child's stderr open long after the child itself ended (ssh's ControlPersist master), and no owner
    /// should wait for that.
    void drainInto(int fd, std::shared_ptr<StderrTail> tail)
    {
        std::thread { [fd, tail = std::move(tail)]() {
            auto buffer = std::array<char, 1024> {};
            while (true)
            {
                auto const count = ::read(fd, buffer.data(), buffer.size());
                if (count > 0)
                    tail->append(std::string_view { buffer.data(), static_cast<std::size_t>(count) });
                else if (count == 0 || errno != EINTR)
                    break;
            }
            ::close(fd);
            tail->end();
        } }.detach();
    }

    /// @return The exit code @p status reports: the exit status, or 128 + the signal that ended it.
    [[nodiscard]] int exitCodeOf(int status) noexcept
    {
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        return WEXITSTATUS(status);
    }

    class PosixStdioChild final: public IStdioChild
    {
      public:
        PosixStdioChild(pid_t pid, std::shared_ptr<StderrTail> tail): _pid { pid }, _tail { std::move(tail) }
        {
        }
        ~PosixStdioChild() override { terminate(); }
        PosixStdioChild(PosixStdioChild const&) = delete;
        PosixStdioChild& operator=(PosixStdioChild const&) = delete;
        PosixStdioChild(PosixStdioChild&&) = delete;
        PosixStdioChild& operator=(PosixStdioChild&&) = delete;

        [[nodiscard]] std::string stderrTail() const override { return _tail->text(); }

        [[nodiscard]] std::optional<int> exitCode(std::chrono::milliseconds wait) override
        {
            auto const deadline = std::chrono::steady_clock::now() + wait;
            if (!awaitExit(deadline))
                return std::nullopt;
            _tail->awaitEnd(deadline);
            return _exitCode;
        }

        void terminate() noexcept override
        {
            if (_exitCode)
                return;
            ::kill(_pid, SIGTERM);
            if (awaitExit(std::chrono::steady_clock::now() + TerminateGrace))
                return;
            ::kill(_pid, SIGKILL);
            auto status = 0;
            // Waited for until reaped: a signal that interrupts the wait would leave it a zombie.
            while (::waitpid(_pid, &status, 0) < 0)
                if (errno != EINTR)
                    return;
            _exitCode = exitCodeOf(status);
        }

      private:
        /// Reaps the child if it ended by @p deadline, looking at it with a backing-off poll.
        /// @return Whether it has ended.
        [[nodiscard]] bool awaitExit(std::chrono::steady_clock::time_point deadline) noexcept
        {
            auto pause = std::chrono::milliseconds { 1 };
            while (!_exitCode)
            {
                auto status = 0;
                auto const reaped = ::waitpid(_pid, &status, WNOHANG);
                if (reaped == _pid)
                    _exitCode = exitCodeOf(status);
                // Reaped by someone else (a SIGCHLD handler): it ended, and its status is lost.
                else if (reaped < 0 && errno != EINTR)
                    _exitCode = ExitStatusLost;
                else if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                else
                {
                    std::this_thread::sleep_for(pause);
                    pause = std::min(pause * 2, std::chrono::milliseconds { 50 });
                }
            }
            return true;
        }

        pid_t _pid;
        std::shared_ptr<StderrTail> _tail;
        std::optional<int> _exitCode;
    };

    class PosixStdioSpawner final: public IStdioSpawner
    {
      public:
        explicit PosixStdioSpawner(vtpty::ProgramSearch search): _search { std::move(search) } {}

        [[nodiscard]] std::expected<SpawnedStdioChild, StdioSpawnError> spawn(
            core::net::EventLoop& loop, std::span<std::string const> argv) override
        {
            if (argv.empty())
                return std::unexpected { StdioSpawnError { .kind = SpawnFailure::SystemError,
                                                           .message = "no program to start" } };
            auto const image = vtpty::findProgram(argv.front(), _search);
            if (!image)
                return std::unexpected { StdioSpawnError {
                    .kind = SpawnFailure::ProgramNotFound,
                    .message = std::format("{} was not found on PATH ({})", argv.front(), _search.path) } };

            auto pair = std::array<int, 2> { -1, -1 };
            if (::socketpair(AF_UNIX, SOCK_STREAM | SocketPairFlags, 0, pair.data()) < 0)
                return std::unexpected { systemError("socketpair", errno) };
            auto nearEnd = vtpty::OwnedFd { pair[0] };
            auto const farEnd = vtpty::OwnedFd { pair[1] };

            auto errorPipe = std::array<int, 2> { -1, -1 };
            if (auto const error = makePipe(errorPipe); error != 0)
                return std::unexpected { systemError("pipe", error) };
            auto errorRead = vtpty::OwnedFd { errorPipe[0] };
            auto const errorWrite = vtpty::OwnedFd { errorPipe[1] };

            for (auto const fd: { nearEnd.get(), farEnd.get(), errorRead.get(), errorWrite.get() })
                if (auto const error = setCloseOnExec(fd); error != 0)
                    return std::unexpected { systemError("fcntl", error) };

            auto pid = pid_t { -1 };
            if (auto const spawned = spawnWithStdio(*image, argv, farEnd.get(), errorWrite.get(), pid);
                spawned != 0)
                return std::unexpected { systemError(std::format("cannot start {}", *image), spawned) };

            auto tail = std::make_shared<StderrTail>(StderrTailLimit);
            drainInto(errorRead.release(), tail);
            auto process = std::make_unique<PosixStdioChild>(pid, std::move(tail));

            auto transport = core::net::adoptFd(loop, nearEnd.get());
            if (!transport)
                return std::unexpected { StdioSpawnError {
                    .kind = SpawnFailure::SystemError,
                    .message =
                        std::format("cannot adopt the child's socket: {}", transport.error().toString()) } };
            (void) nearEnd.release(); // the socket owns it now
            return SpawnedStdioChild { .transport = std::move(*transport), .process = std::move(process) };
        }

      private:
        /// posix_spawn()s @p image with @p stdio as its stdin and stdout and @p stderrFd as its stderr.
        /// No fork in this multi-threaded process, so there is no window in which a child may only call
        /// async-signal-safe functions.
        /// @return 0, or the error number the spawn failed with.
        [[nodiscard]] static int spawnWithStdio(std::string const& image,
                                                std::span<std::string const> arguments,
                                                int stdio,
                                                int stderrFd,
                                                pid_t& pid)
        {
            auto argv = std::vector<char*> {};
            argv.reserve(arguments.size() + 1);
            auto copies = std::vector<std::string>(arguments.begin(), arguments.end());
            for (auto& argument: copies)
                argv.push_back(argument.data());
            argv.push_back(nullptr);

            auto actions = posix_spawn_file_actions_t {};
            if (auto const error = ::posix_spawn_file_actions_init(&actions); error != 0)
                return error;
            auto attributes = posix_spawnattr_t {};
            if (auto const error = ::posix_spawnattr_init(&attributes); error != 0)
            {
                ::posix_spawn_file_actions_destroy(&actions);
                return error;
            }

            // dup2 clears close-on-exec on the copy only: the originals still close at exec.
            ::posix_spawn_file_actions_adddup2(&actions, stdio, STDIN_FILENO);
            ::posix_spawn_file_actions_adddup2(&actions, stdio, STDOUT_FILENO);
            ::posix_spawn_file_actions_adddup2(&actions, stderrFd, STDERR_FILENO);

            // Whatever this process blocks or ignores (a daemon blocks its shutdown signals, a GUI
            // ignores SIGPIPE) is not the child's to inherit.
            auto none = sigset_t {};
            sigemptyset(&none);
            auto defaults = sigset_t {};
            sigemptyset(&defaults);
            sigaddset(&defaults, SIGPIPE);
            sigaddset(&defaults, SIGINT);
            sigaddset(&defaults, SIGTERM);
            sigaddset(&defaults, SIGHUP);
            ::posix_spawnattr_setsigmask(&attributes, &none);
            ::posix_spawnattr_setsigdefault(&attributes, &defaults);
            ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

            auto const error =
                ::posix_spawn(&pid, image.c_str(), &actions, &attributes, argv.data(), environ);
            ::posix_spawnattr_destroy(&attributes);
            ::posix_spawn_file_actions_destroy(&actions);
            return error;
        }

        vtpty::ProgramSearch _search;
    };
} // namespace

std::unique_ptr<IStdioSpawner> makeSystemStdioSpawner(vtpty::ProgramSearch search)
{
    return std::make_unique<PosixStdioSpawner>(std::move(search));
}

} // namespace vthost

#endif // !_WIN32
