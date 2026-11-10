// SPDX-License-Identifier: Apache-2.0
#include <vthost/StdioChild.hpp>

#ifndef _WIN32

    #include <vtpty/OwnedFd.hpp>
    #include <vtpty/PosixSpawn.hpp>

    #include <core/net/Sockets.hpp>

    #include <sys/socket.h>
    #include <sys/wait.h>

    #include <algorithm>
    #include <array>
    #include <cerrno>
    #include <csignal>
    #include <format>
    #include <initializer_list>
    #include <string_view>
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
            if (_isReaped)
                return;
            // The whole group: ssh's own children (a ProxyJump's hop is a ProxyCommand child that asks the
            // askpass endpoint itself) do not end with it, and must not outlive the attempt.
            if (!_exitCode)
            {
                ::kill(-_pid, SIGTERM);
                (void) awaitExit(std::chrono::steady_clock::now() + TerminateGrace);
            }
            // Swept before the leader is reaped: until then its zombie keeps the group id from being
            // reused, so this reaches only our group, and also a member that outlived an exited leader.
            ::kill(-_pid, SIGKILL);
            reap();
        }

      private:
        /// Looks at whether the child ended by @p deadline, with a backing-off poll, without reaping it: a
        /// zombie leader keeps its process group id reserved.
        /// @return Whether it has ended; its exit code is then known.
        [[nodiscard]] bool awaitExit(std::chrono::steady_clock::time_point deadline) noexcept
        {
            auto pause = std::chrono::milliseconds { 1 };
            while (!_exitCode)
            {
                auto info = siginfo_t {};
                if (::waitid(P_PID, static_cast<id_t>(_pid), &info, WEXITED | WNOHANG | WNOWAIT) < 0)
                {
                    // Reaped by someone else (a SIGCHLD handler): it ended, and its status is lost.
                    if (errno == ECHILD)
                    {
                        _exitCode = ExitStatusLost;
                        _isReaped = true;
                        break;
                    }
                }
                else if (info.si_pid == _pid)
                {
                    _exitCode = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(pause);
                pause = std::min(pause * 2, std::chrono::milliseconds { 50 });
            }
            return true;
        }

        /// Reaps the leader, waiting for it until it is: a signal that interrupts the wait would leave a
        /// zombie.
        void reap() noexcept
        {
            auto status = 0;
            while (::waitpid(_pid, &status, 0) < 0)
                if (errno != EINTR)
                {
                    if (!_exitCode)
                        _exitCode = ExitStatusLost;
                    _isReaped = true;
                    return;
                }
            if (!_exitCode)
                _exitCode = exitCodeOf(status);
            _isReaped = true;
        }

        pid_t _pid;
        std::shared_ptr<StderrTail> _tail;
        std::optional<int> _exitCode; ///< Set once the child is seen to have ended.
        bool _isReaped = false;       ///< Whether waitpid() collected it, which ends its group id's hold.
    };

    class PosixStdioSpawner final: public IStdioSpawner
    {
      public:
        explicit PosixStdioSpawner(vtpty::ProgramSearch search): _search { std::move(search) } {}

        [[nodiscard]] std::expected<SpawnedStdioChild, StdioSpawnError> spawn(
            core::net::EventLoop& loop,
            std::span<std::string const> argv,
            SpawnEnvironment const& environment) override
        {
            if (argv.empty())
                return std::unexpected { StdioSpawnError { .kind = SpawnFailure::SystemError,
                                                           .message = "no program to start" } };
            if (auto const bad = invalidEnvironmentName(environment))
                return std::unexpected { StdioSpawnError {
                    .kind = SpawnFailure::SystemError,
                    .message = std::format("'{}' cannot name an environment variable", *bad) } };
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
            if (auto const spawned =
                    spawnWithStdio(*image, argv, environment, farEnd.get(), errorWrite.get(), pid);
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
        /// @return This process's environment as `NAME=VALUE` entries, @p environment applied on top.
        [[nodiscard]] static std::vector<std::string> mergedEnvironment(SpawnEnvironment const& environment)
        {
            auto inherited = std::vector<std::string> {};
            for (auto const* entry = environ; entry != nullptr && *entry != nullptr; ++entry)
                inherited.emplace_back(*entry);
            return mergeEnvironment<char>(
                inherited, environment.set, NameComparison::CaseSensitive, EntryOrder::AsFound);
        }

        /// posix_spawn()s @p image with @p stdio as its stdin and stdout and @p stderrFd as its stderr.
        /// No fork in this multi-threaded process, so there is no window in which a child may only call
        /// async-signal-safe functions.
        /// @return 0, or the error number the spawn failed with.
        [[nodiscard]] static int spawnWithStdio(std::string const& image,
                                                std::span<std::string const> arguments,
                                                SpawnEnvironment const& environment,
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

            auto entries = mergedEnvironment(environment);
            auto envp = std::vector<char*> {};
            envp.reserve(entries.size() + 1);
            for (auto& entry: entries)
                envp.push_back(entry.data());
            envp.push_back(nullptr);

            auto actions = vtpty::SpawnFileActions::create();
            if (!actions)
                return actions.error().value();
            auto attributes = vtpty::SpawnAttributes::create();
            if (!attributes)
                return attributes.error().value();
            if (auto const reset = vtpty::resetSignals(*attributes); !reset)
                return reset.error().value();
            // The child leads a process group of its own, which terminate() ends as a whole.
            auto flags = short {};
            ::posix_spawnattr_getflags(attributes->get(), &flags);
            if (auto const error = ::posix_spawnattr_setflags(
                    attributes->get(), static_cast<short>(flags | POSIX_SPAWN_SETPGROUP));
                error != 0)
                return error;
            if (auto const error = ::posix_spawnattr_setpgroup(attributes->get(), 0); error != 0)
                return error;

            // dup2 clears close-on-exec on the copy only: the originals still close at exec.
            ::posix_spawn_file_actions_adddup2(actions->get(), stdio, STDIN_FILENO);
            ::posix_spawn_file_actions_adddup2(actions->get(), stdio, STDOUT_FILENO);
            ::posix_spawn_file_actions_adddup2(actions->get(), stderrFd, STDERR_FILENO);

            return ::posix_spawn(
                &pid, image.c_str(), actions->get(), attributes->get(), argv.data(), envp.data());
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
