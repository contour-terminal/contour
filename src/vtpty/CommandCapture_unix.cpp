// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandCapture.hpp>
#include <vtpty/CommandCaptureStop.hpp>
#include <vtpty/OwnedFd.hpp>

#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>

// POSIX requires no header to declare `environ`; Process_unix.cpp explains the per-platform story.
#ifdef __APPLE__
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" char** environ;
#endif

namespace vtpty
{

namespace
{
    using Clock = std::chrono::steady_clock;

    /// Granularity at which waits re-check the stop token and the deadline.
    constexpr auto PollSlice = std::chrono::milliseconds { 50 };

    /// Granularity of the wait for the shell to exit once its stdout has closed.
    constexpr auto ReapSlice = std::chrono::milliseconds { 10 };

    /// How long a killed shell is given to be reaped before it is abandoned.
    constexpr auto KillGracePeriod = std::chrono::milliseconds { 1000 };

    [[nodiscard]] std::error_code errnoError(int code) noexcept
    {
        return std::error_code { code, std::system_category() };
    }

    [[nodiscard]] std::error_code lastError() noexcept
    {
        return errnoError(errno);
    }

    /// Owns an initialized posix_spawn_file_actions_t.
    class FileActions
    {
      public:
        FileActions() noexcept: _initResult { posix_spawn_file_actions_init(&_actions) } {}
        FileActions(FileActions const&) = delete;
        FileActions& operator=(FileActions const&) = delete;
        FileActions(FileActions&&) = delete;
        FileActions& operator=(FileActions&&) = delete;
        ~FileActions()
        {
            if (_initResult == 0)
                posix_spawn_file_actions_destroy(&_actions);
        }

        /// @return 0 when the object is usable; otherwise the errno value of the failed init.
        [[nodiscard]] int initResult() const noexcept { return _initResult; }
        [[nodiscard]] posix_spawn_file_actions_t* get() noexcept { return &_actions; }

      private:
        posix_spawn_file_actions_t _actions {};
        int _initResult;
    };

    /// Owns an initialized posix_spawnattr_t.
    class SpawnAttributes
    {
      public:
        SpawnAttributes() noexcept: _initResult { posix_spawnattr_init(&_attributes) } {}
        SpawnAttributes(SpawnAttributes const&) = delete;
        SpawnAttributes& operator=(SpawnAttributes const&) = delete;
        SpawnAttributes(SpawnAttributes&&) = delete;
        SpawnAttributes& operator=(SpawnAttributes&&) = delete;
        ~SpawnAttributes()
        {
            if (_initResult == 0)
                posix_spawnattr_destroy(&_attributes);
        }

        /// @return 0 when the object is usable; otherwise the errno value of the failed init.
        [[nodiscard]] int initResult() const noexcept { return _initResult; }
        [[nodiscard]] posix_spawnattr_t* get() noexcept { return &_attributes; }

      private:
        posix_spawnattr_t _attributes {};
        int _initResult;
    };

    /// A pipe whose both ends close on exec.
    struct Pipe
    {
        OwnedFd read;
        OwnedFd write;
    };

    [[nodiscard]] std::expected<Pipe, std::error_code> makePipe()
    {
        auto fds = std::array<int, 2> { -1, -1 };
#if defined(__linux__) || defined(__FreeBSD__)
        if (pipe2(fds.data(), O_CLOEXEC) != 0)
            return std::unexpected(lastError());
        return Pipe { OwnedFd { fds[0] }, OwnedFd { fds[1] } };
#else
        if (pipe(fds.data()) != 0)
            return std::unexpected(lastError());
        auto result = Pipe { OwnedFd { fds[0] }, OwnedFd { fds[1] } };
        for (auto const fd: fds)
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
                return std::unexpected(lastError());
        return result;
#endif
    }

    /// A running shell, leader of its own process group, and the read end of its stdout.
    struct Spawned
    {
        pid_t pid;      ///< The shell, and the id of its process group.
        OwnedFd output; ///< Read end of the shell's stdout.
    };

    [[nodiscard]] std::unexpected<CommandCaptureError> spawnFailed(std::error_code cause)
    {
        return std::unexpected(CommandCaptureError { CommandCaptureError::Kind::SpawnFailed, cause });
    }

    /// Starts `<shell> -c <command>` as a process-group leader, stdout piped to us, stdin and stderr on
    /// /dev/null.
    [[nodiscard]] std::expected<Spawned, std::error_code> spawnShell(std::string const& shell,
                                                                     std::string const& command)
    {
        auto pipe = makePipe();
        if (!pipe)
            return std::unexpected(pipe.error());

        auto actions = FileActions {};
        auto attributes = SpawnAttributes {};
        if (actions.initResult() != 0)
            return std::unexpected(errnoError(actions.initResult()));
        if (attributes.initResult() != 0)
            return std::unexpected(errnoError(attributes.initResult()));

        // The calling thread may block signals (the status-line pool does, see ContourGuiApp); the
        // shell must start with an empty mask and default dispositions, as UnixPty does for its child.
        auto noBlockedSignals = sigset_t {};
        sigemptyset(&noBlockedSignals);
        auto allSignals = sigset_t {};
        sigfillset(&allSignals);
        // SIGKILL and SIGSTOP cannot have a disposition; keep them out so no platform rejects the set.
        sigdelset(&allSignals, SIGKILL);
        sigdelset(&allSignals, SIGSTOP);

        // dup2 clears close-on-exec on stdout; both pipe ends close on exec, so the child keeps only fd 1.
        auto const prepared = std::array<int, 7> {
            posix_spawn_file_actions_adddup2(actions.get(), pipe->write.get(), STDOUT_FILENO),
            posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0),
            posix_spawn_file_actions_addopen(actions.get(), STDERR_FILENO, "/dev/null", O_WRONLY, 0),
            posix_spawnattr_setflags(attributes.get(),
                                     POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF),
            posix_spawnattr_setsigmask(attributes.get(), &noBlockedSignals),
            posix_spawnattr_setsigdefault(attributes.get(), &allSignals),
            posix_spawnattr_setpgroup(attributes.get(), 0),
        };
        if (auto const failed = std::ranges::find_if(prepared, [](int rc) { return rc != 0; });
            failed != prepared.end())
            return std::unexpected(errnoError(*failed));

        // posix_spawn takes a non-const argv but never modifies it.
        auto shellArg = shell;
        auto commandArg = command;
        auto flagArg = std::string { "-c" };
        auto const argv =
            std::array<char*, 4> { shellArg.data(), flagArg.data(), commandArg.data(), nullptr };
        auto pid = pid_t { 0 };
        if (auto const rc =
                posix_spawn(&pid, shell.c_str(), actions.get(), attributes.get(), argv.data(), environ);
            rc != 0)
            return std::unexpected(errnoError(rc));

        // Our copy of the write end closes on return; otherwise the pipe never reports EOF.
        return Spawned { pid, std::move(pipe->read) };
    }

    /// @return Why the run must be abandoned (stop requested or deadline passed), if it must.
    [[nodiscard]] std::optional<CommandCaptureError::Kind> abandonReason(RunStop const& stop,
                                                                         Clock::time_point deadline)
    {
        if (stop.requested())
            return CommandCaptureError::Kind::Cancelled;
        if (Clock::now() >= deadline)
            return CommandCaptureError::Kind::TimedOut;
        return std::nullopt;
    }

    /// @return The time left until @p deadline, at most @p slice, never negative.
    [[nodiscard]] std::chrono::milliseconds sliceUntil(Clock::time_point deadline,
                                                       std::chrono::milliseconds slice)
    {
        auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        return std::clamp(remaining, std::chrono::milliseconds { 0 }, slice);
    }

    /// Kills the shell's whole process group, then reaps the shell, waiting at most KillGracePeriod.
    /// A shell stuck in uninterruptible sleep cannot be reaped yet; abandoning it as a zombie is the
    /// lesser evil compared to pinning this thread (and so the shutdown join) on it forever.
    void killAndReap(pid_t pid) noexcept
    {
        kill(-pid, SIGKILL);
        auto const giveUpAt = Clock::now() + KillGracePeriod;
        while (true)
        {
            auto const rc = waitpid(pid, nullptr, WNOHANG);
            if (rc == pid || (rc < 0 && errno != EINTR))
                return; // reaped, or already gone
            if (Clock::now() >= giveUpAt)
                return;
            std::this_thread::sleep_for(ReapSlice);
        }
    }

    /// Reads the shell's stdout to EOF, keeping at most MaxCapturedOutput bytes.
    [[nodiscard]] std::expected<std::string, CommandCaptureError::Kind> drainOutput(
        int fd, Clock::time_point deadline, RunStop const& stop)
    {
        auto captured = std::string {};
        auto buffer = std::array<char, CaptureReadChunkSize> {};
        auto eof = false;
        while (!eof)
        {
            if (auto const reason = abandonReason(stop, deadline))
                return std::unexpected(*reason);

            // Returns at once while data is available; sleeps only when there is none.
            auto pollFd = pollfd { fd, POLLIN, 0 };
            auto const ready = poll(&pollFd, 1, static_cast<int>(sliceUntil(deadline, PollSlice).count()));
            if (ready < 0)
            {
                if (errno == EINTR)
                    continue;
                break; // unexpected poll failure: treat as the end of the output
            }
            if (ready == 0)
                continue;

            auto const count = read(fd, buffer.data(), buffer.size());
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                break; // unexpected read failure: treat as the end of the output
            }
            eof = count == 0;
            if (captured.size() < MaxCapturedOutput) // beyond the cap bytes are drained and dropped
                captured.append(buffer.data(),
                                std::min(static_cast<size_t>(count), MaxCapturedOutput - captured.size()));
        }
        return captured;
    }

    /// Waits for the shell to exit and reaps it, honouring stop and deadline.
    [[nodiscard]] std::expected<void, CommandCaptureError::Kind> awaitExit(pid_t pid,
                                                                           Clock::time_point deadline,
                                                                           RunStop const& stop)
    {
        while (true)
        {
            if (auto const reason = abandonReason(stop, deadline))
                return std::unexpected(*reason);
            auto const rc = waitpid(pid, nullptr, WNOHANG);
            if (rc == pid || (rc < 0 && errno != EINTR))
                return {}; // reaped, or already gone
            std::this_thread::sleep_for(sliceUntil(deadline, ReapSlice));
        }
    }
} // namespace

std::expected<std::string, CommandCaptureError> SystemCommandCapture::run(std::string_view command,
                                                                          std::chrono::milliseconds timeout,
                                                                          core::async::StopToken stop)
{
    auto const runStop = RunStop { std::move(stop), _shutdown };
    // A run after shutdown never spawns anything.
    if (runStop.requested())
        return std::unexpected(CommandCaptureError { CommandCaptureError::Kind::Cancelled, {} });
    auto const deadline = Clock::now() + timeout;

    auto const spawned = spawnShell(_shell, std::string { command });
    if (!spawned)
        return spawnFailed(spawned.error());

    auto const captured = drainOutput(spawned->output.get(), deadline, runStop);
    if (!captured)
    {
        killAndReap(spawned->pid);
        return std::unexpected(CommandCaptureError { captured.error(), {} });
    }

    if (auto const exited = awaitExit(spawned->pid, deadline, runStop); !exited)
    {
        killAndReap(spawned->pid);
        return std::unexpected(CommandCaptureError { exited.error(), {} });
    }
    // The shell is reaped; kill whatever it left in its group, as the Windows job object does.
    kill(-spawned->pid, SIGKILL);
    return firstLineOf(*captured);
}

} // namespace vtpty
