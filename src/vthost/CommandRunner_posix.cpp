// SPDX-License-Identifier: Apache-2.0
#include <vthost/CommandRunner.hpp>

#ifndef _WIN32

    #include <sys/wait.h>

    #include <array>
    #include <cerrno>
    #include <memory>
    #include <utility>
    #include <vector>

    #include <fcntl.h>
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

    /// Owns a posix_spawn_file_actions_t.
    class FileActions
    {
      public:
        FileActions() { ::posix_spawn_file_actions_init(&_actions); }
        FileActions(FileActions const&) = delete;
        FileActions& operator=(FileActions const&) = delete;
        FileActions(FileActions&&) = delete;
        FileActions& operator=(FileActions&&) = delete;
        ~FileActions() { ::posix_spawn_file_actions_destroy(&_actions); }

        [[nodiscard]] posix_spawn_file_actions_t* get() noexcept { return &_actions; }

      private:
        posix_spawn_file_actions_t _actions {};
    };

    class NativeCommandRunner final: public CommandRunner
    {
      public:
        [[nodiscard]] std::expected<CommandOutcome, std::error_code> run(
            std::span<std::string const> argv) override
        {
            if (argv.empty())
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));

            auto pipe = makePipe();
            if (!pipe)
                return std::unexpected(pipe.error());
            auto& [readEnd, writeEnd] = *pipe;

            auto actions = FileActions {};
            ::posix_spawn_file_actions_adddup2(actions.get(), writeEnd.get(), STDOUT_FILENO);
            ::posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            ::posix_spawn_file_actions_addopen(actions.get(), STDERR_FILENO, "/dev/null", O_WRONLY, 0);

            auto arguments = std::vector<char*> {};
            for (auto const& argument: argv)
                arguments.push_back(const_cast<char*>(
                    argument.c_str())); // posix_spawnp takes a non-const argv but never writes it
            arguments.push_back(nullptr);

            auto pid = pid_t {};
            auto const spawned =
                ::posix_spawnp(&pid, arguments.front(), actions.get(), nullptr, arguments.data(), environ);
            if (spawned != 0)
                return std::unexpected(std::error_code { spawned, std::generic_category() });
            writeEnd.reset();

            auto outcome = CommandOutcome {};
            auto buffer = std::array<char, 4096> {};
            while (true)
            {
                auto const count = ::read(readEnd.get(), buffer.data(), buffer.size());
                if (count > 0)
                    outcome.standardOutput.append(buffer.data(), static_cast<size_t>(count));
                else if (count < 0 && errno == EINTR)
                    continue;
                else
                    break;
            }

            auto status = 0;
            while (::waitpid(pid, &status, 0) < 0)
                if (errno != EINTR)
                    return std::unexpected(lastError());
            outcome.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            return outcome;
        }
    };
} // namespace

std::unique_ptr<CommandRunner> makeNativeCommandRunner()
{
    return std::make_unique<NativeCommandRunner>();
}

} // namespace vthost

#endif // !_WIN32
