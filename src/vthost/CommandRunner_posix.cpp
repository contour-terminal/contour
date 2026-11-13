// SPDX-License-Identifier: Apache-2.0
#include <vthost/CommandRunner.hpp>

#ifndef _WIN32

    #include <vtpty/EnvironmentBlock.hpp>
    #include <vtpty/PosixSpawn.hpp>
    #include <vtpty/ProgramLookup.hpp>

    #include <core/platform/NativeFileSystem.hpp>

    #include <sys/wait.h>

    #include <array>
    #include <cerrno>
    #include <chrono>
    #include <csignal>
    #include <cstdint>
    #include <memory>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <utility>
    #include <vector>

    #include <fcntl.h>
    #include <poll.h>
    #include <spawn.h>
    #include <unistd.h>

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
    /// Owns a file descriptor.
    class Descriptor
    {
      public:
        Descriptor() = default;
        explicit Descriptor(int fd) noexcept: _fd { fd } {}
        Descriptor(Descriptor const&) = delete;
        Descriptor& operator=(Descriptor const&) = delete;
        Descriptor(Descriptor&& other) noexcept: _fd { std::exchange(other._fd, -1) } {}
        Descriptor& operator=(Descriptor&& other) noexcept
        {
            reset();
            _fd = std::exchange(other._fd, -1);
            return *this;
        }
        ~Descriptor() { reset(); }

        [[nodiscard]] int get() const noexcept { return _fd; }
        void reset() noexcept
        {
            if (_fd >= 0)
                ::close(std::exchange(_fd, -1));
        }

      private:
        int _fd = -1;
    };

    [[nodiscard]] std::error_code lastError() noexcept
    {
        return std::error_code { errno, std::generic_category() };
    }

    /// A pipe whose both ends close on exec: the child keeps only the end dup2'd onto its stdout.
    [[nodiscard]] std::expected<std::array<Descriptor, 2>, std::error_code> makePipe()
    {
        auto fds = std::array<int, 2> {};
        if (::pipe(fds.data()) != 0)
            return std::unexpected(lastError());
        auto ends = std::array<Descriptor, 2> { Descriptor { fds[0] }, Descriptor { fds[1] } };
        for (auto const& end: ends)
            if (::fcntl(end.get(), F_SETFD, FD_CLOEXEC) != 0)
                return std::unexpected(lastError());
        return ends;
    }

    /// @return Pointers to @p strings' characters, ended by a null pointer, as exec() takes them. The
    ///         functions they are passed to take non-const pointers but never write through them.
    [[nodiscard]] std::vector<char*> pointersTo(std::span<std::string const> strings)
    {
        auto pointers = std::vector<char*> {};
        pointers.reserve(strings.size() + 1);
        for (auto const& entry: strings)
            pointers.push_back(const_cast<char*>(entry.c_str()));
        pointers.push_back(nullptr);
        return pointers;
    }

    class NativeCommandRunner final: public CommandRunner
    {
      public:
        /// @param environment What every command runs under ("NAME=VALUE"), its program looked up on that
        ///        environment's PATH; std::nullopt for this process's own, as it stands at each run.
        explicit NativeCommandRunner(std::optional<std::vector<std::string>> environment):
            _environment { std::move(environment) }
        {
        }

        [[nodiscard]] std::expected<CommandOutcome, std::error_code> run(
            std::span<std::string const> argv) override
        {
            return execute(argv, std::nullopt);
        }

        [[nodiscard]] std::expected<CommandOutcome, std::error_code> runWithin(
            std::span<std::string const> argv, std::chrono::milliseconds deadline) override
        {
            return execute(argv, deadline);
        }

      private:
        using Clock = std::chrono::steady_clock;

        /// Kills @p pid and reaps it.
        static void killAndReap(pid_t pid) noexcept
        {
            ::kill(pid, SIGKILL);
            auto status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
        }

        /// Whether a child ended by itself before its deadline.
        enum class ChildEnd : uint8_t
        {
            Running = 0, ///< It was still running at the deadline, and is not reaped.
            Ended,       ///< It ended, and is reaped (or was gone already).
        };

        /// Reaps @p pid if it ends by itself before @p end.
        /// @param status Its wait status, once it Ended.
        /// @return Whether it ended in time.
        [[nodiscard]] static ChildEnd reapBefore(pid_t pid, Clock::time_point end, int& status)
        {
            using namespace std::chrono_literals;
            while (true)
            {
                auto const reaped = ::waitpid(pid, &status, WNOHANG);
                if (reaped == pid)
                    return ChildEnd::Ended;
                if (reaped < 0 && errno != EINTR)
                    return ChildEnd::Ended; // Gone already; nothing to wait for.
                if (Clock::now() >= end)
                    return ChildEnd::Running;
                ::poll(nullptr, 0, 5);
            }
        }

        [[nodiscard]] std::expected<CommandOutcome, std::error_code> execute(
            std::span<std::string const> argv, std::optional<std::chrono::milliseconds> deadline)
        {
            if (argv.empty())
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));

            auto pipe = makePipe();
            if (!pipe)
                return std::unexpected(pipe.error());
            auto& [readEnd, writeEnd] = *pipe;

            auto actions = vtpty::SpawnFileActions::create();
            if (!actions)
                return std::unexpected(actions.error());
            ::posix_spawn_file_actions_adddup2(actions->get(), writeEnd.get(), STDOUT_FILENO);
            ::posix_spawn_file_actions_addopen(actions->get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            ::posix_spawn_file_actions_addopen(actions->get(), STDERR_FILENO, "/dev/null", O_WRONLY, 0);

            // Neither the signals the calling thread blocks -- the daemon blocks SIGINT and SIGTERM -- nor
            // what this process ignores are the command's to inherit.
            auto attributes = vtpty::SpawnAttributes::create();
            if (!attributes)
                return std::unexpected(attributes.error());
            if (auto const reset = vtpty::resetSignals(*attributes); !reset)
                return std::unexpected(reset.error());

            auto arguments = pointersTo(argv);
            auto pid = pid_t {};
            auto const spawned = [&] {
                if (!_environment)
                    return ::posix_spawnp(&pid,
                                          arguments.front(),
                                          actions->get(),
                                          attributes->get(),
                                          arguments.data(),
                                          environ);
                // posix_spawnp() would search the live PATH, not the given environment's: looked up here
                // instead, and the result spawned as it is, so nothing of the live environment is read.
                auto const image = imageFor(argv.front());
                if (!image)
                    return ENOENT;
                auto environment = pointersTo(*_environment);
                return ::posix_spawn(&pid,
                                     image->c_str(),
                                     actions->get(),
                                     attributes->get(),
                                     arguments.data(),
                                     environment.data());
            }();
            if (spawned != 0)
                return std::unexpected(std::error_code { spawned, std::generic_category() });
            writeEnd.reset();

            auto const end = deadline ? Clock::now() + *deadline : Clock::time_point::max();
            auto outcome = CommandOutcome {};
            auto buffer = std::array<char, 4096> {};
            while (true)
            {
                if (deadline)
                {
                    auto const left =
                        std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now()).count();
                    auto waiting = pollfd { .fd = readEnd.get(), .events = POLLIN, .revents = 0 };
                    auto const ready = left > 0 ? ::poll(&waiting, 1, static_cast<int>(left)) : 0;
                    if (ready < 0 && errno == EINTR)
                        continue;
                    if (ready <= 0)
                    {
                        killAndReap(pid);
                        return std::unexpected(std::make_error_code(std::errc::timed_out));
                    }
                }
                auto const count = ::read(readEnd.get(), buffer.data(), buffer.size());
                if (count > 0)
                    outcome.standardOutput.append(buffer.data(), static_cast<size_t>(count));
                else if (count < 0 && errno == EINTR)
                    continue;
                else
                    break;
            }

            auto status = 0;
            if (deadline)
            {
                // The output is complete, but the command may still not end: the deadline bounds it too.
                if (reapBefore(pid, end, status) == ChildEnd::Running)
                {
                    killAndReap(pid);
                    return std::unexpected(std::make_error_code(std::errc::timed_out));
                }
            }
            else
                while (::waitpid(pid, &status, 0) < 0)
                    if (errno != EINTR)
                        return std::unexpected(lastError());
            outcome.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            return outcome;
        }

        /// @param program The program as named, a bare name or a path.
        /// @return The file to spawn for @p program, looked up on the given environment's PATH as execvp()
        ///         would; std::nullopt for a bare name found nowhere on it.
        [[nodiscard]] std::optional<std::string> imageFor(std::string const& program) const
        {
            auto const path =
                vtpty::environmentValue(*_environment, "PATH", vtpty::EnvironmentNames::CaseSensitive);
            auto image =
                vtpty::resolveExecutablePath(program,
                                             vtpty::execSearchPath(path.transform([](std::string_view value) {
                                                 return std::string { value };
                                             })),
                                             core::platform::NativeFileSystem::instance());
            if (image == program && !program.contains('/'))
                return std::nullopt;
            return image;
        }

        std::optional<std::vector<std::string>> _environment;
    };
} // namespace

std::unique_ptr<CommandRunner> makeNativeCommandRunner()
{
    return std::make_unique<NativeCommandRunner>(std::nullopt);
}

std::unique_ptr<CommandRunner> makeNativeCommandRunner(std::vector<std::string> environment)
{
    return std::make_unique<NativeCommandRunner>(std::move(environment));
}

} // namespace vthost

#endif // !_WIN32
