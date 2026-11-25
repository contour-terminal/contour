// SPDX-License-Identifier: Apache-2.0
//
// The launcher on FreeBSD. pdfork() with PD_CLOEXEC: the child is a process descriptor from the first
// instruction, which kill() signals (pdkill), the wait set polls (it hangs up when the process exits), and
// whose close ends the process -- nothing ever addresses it by a pid that could be reused. Everything the
// child needs is prepared before the fork, so that the child of this multi-threaded process calls only
// async-signal-safe functions: dup2(), closefrom(), chdir(), sigaction(), sigprocmask() and execve(), and
// _exit(127) on any failure.
#include <sys/procdesc.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <csignal>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <vtextension/wasm/remote/process/PosixLaunch.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

namespace
{
    using posix::UniqueFd;

    /// The exit code of a child that could not become the program it was to run.
    constexpr auto ExecFailed = 127;

    class FreeBsdProcessHandle final: public IProcessHandle
    {
      public:
        FreeBsdProcessHandle(pid_t pid, UniqueFd descriptor) noexcept:
            _pid { pid }, _descriptor { std::move(descriptor) }
        {
        }

        [[nodiscard]] ProcessId id() const noexcept override { return _pid; }

        void kill() noexcept override
        {
            if (!_status)
                ::pdkill(_descriptor.get(), SIGKILL);
        }

        [[nodiscard]] std::optional<ExitStatus> tryReap() noexcept override
        {
            if (_status)
                return _status;
            auto status = 0;
            // The descriptor holds the pid until it is closed: reaping by pid cannot reach another process.
            while (true)
            {
                auto const reaped = ::waitpid(_pid, &status, WNOHANG);
                if (reaped == 0)
                    return std::nullopt;
                if (reaped == _pid)
                    break;
                if (errno == EINTR)
                    continue;
                _status = ExitStatus { .kind = ExitKind::Exited, .value = -1 };
                return _status;
            }
            _status = posix::exitStatusOfWait(status);
            return _status;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return _descriptor.get(); }

      private:
        pid_t _pid;
        UniqueFd _descriptor;
        std::optional<ExitStatus> _status; ///< Set once reaped.
    };

    /// Makes @p source the child's descriptor @p target, kept across exec. dup2() onto itself would keep the
    /// descriptor's close-on-exec flag, so that case clears it instead. Async-signal-safe.
    /// @return Whether it worked.
    [[nodiscard]] bool inheritAs(int source, int target) noexcept
    {
        if (source == target)
            return ::fcntl(target, F_SETFD, 0) != -1;
        return ::dup2(source, target) != -1;
    }

    /// Becomes @p prepared's program, in the child of pdfork(). Async-signal-safe throughout; never returns.
    [[noreturn]] void becomeChild(posix::PreparedLaunch& prepared,
                                  char* const* argv,
                                  char* const* envp) noexcept
    {
        struct sigaction byDefault {};
        byDefault.sa_handler = SIG_DFL;
        for (auto const signal: std::views::iota(1, NSIG))
            if (signal != SIGKILL && signal != SIGSTOP)
                ::sigaction(signal, &byDefault, nullptr);
        auto empty = sigset_t {};
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);

        if (!inheritAs(prepared.nullDevice.get(), STDIN_FILENO)
            || !inheritAs(prepared.nullDevice.get(), STDOUT_FILENO)
            || (prepared.standardError.get() != -1
                && !inheritAs(prepared.standardError.get(), STDERR_FILENO)))
            ::_exit(ExecFailed);
        auto child = posix::FirstChildDescriptor;
        for (auto const& parked: prepared.parked)
            if (!inheritAs(parked.get(), child++))
                ::_exit(ExecFailed);
        ::closefrom(prepared.childEnd());
        if (::chdir("/") == -1)
            ::_exit(ExecFailed);
        ::execve(prepared.executable.c_str(), argv, envp);
        ::_exit(ExecFailed);
    }

    class FreeBsdProcessLauncher final: public IProcessLauncher
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
            LaunchSpec const& spec) override
        {
            auto prepared = posix::prepareLaunch(spec);
            if (!prepared)
                return std::unexpected(prepared.error());
            auto argv = prepared->argv();
            auto envp = prepared->envp();

            auto descriptor = -1;
            auto const pid = ::pdfork(&descriptor, PD_CLOEXEC);
            if (pid == -1)
                return std::unexpected(posix::launchErrorOf(errno));
            if (pid == 0)
                becomeChild(*prepared, argv.data(), envp.data());
            return std::make_unique<FreeBsdProcessHandle>(pid, UniqueFd { descriptor });
        }

        [[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
            core::IClock& clock) override
        {
            return posix::makePollExitWaitSet(clock);
        }
    };
} // namespace

std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher()
{
    return std::make_shared<FreeBsdProcessLauncher>();
}

std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment()
{
    return {};
}

} // namespace vtextension::wasm::remote::process
