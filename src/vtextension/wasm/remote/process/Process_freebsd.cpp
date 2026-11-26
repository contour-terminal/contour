// SPDX-License-Identifier: Apache-2.0
//
// The launcher on FreeBSD. pdfork() with PD_CLOEXEC: the child is a process descriptor from the first
// instruction, which kill() signals (pdkill), the wait set polls (it hangs up when the process exits), and
// whose close ends the process -- nothing ever addresses it by a pid that could be reused. Everything the
// child needs is prepared before the fork, so that the child of this multi-threaded process calls only
// async-signal-safe functions: setsid(), dup2(), closefrom(), chdir(), sigaction(), sigprocmask(), procctl()
// and execve(), and _exit(127) on any failure. It starts as the other launchers' children do: in a session of
// its own, which has no controlling terminal, on the null device but for its stderr, in "/".
#include <sys/procctl.h>
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
#include <tuple>
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

        [[nodiscard]] std::expected<std::optional<ExitStatus>, LaunchError> tryReap() noexcept override
        {
            if (_status)
                return _status;
            // The descriptor holds the pid until it is closed: reaping by pid cannot reach another process.
            auto const reaped = posix::reapChild(_pid);
            if (reaped && *reaped)
                _status = *reaped;
            return reaped;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return _descriptor.get(); }

        [[nodiscard]] std::expected<transport::HandleTarget, LaunchError> handleTarget() override
        {
            // A descriptor travels with its frame: there is nothing to name the process by.
            return transport::HandleTarget::currentProcess();
        }

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

        // A session of its own: no controlling terminal, and no process group a terminal signals. pdfork()'s
        // child is no process group leader, which is all setsid() asks.
        if (::setsid() == -1)
            ::_exit(ExecFailed);
        if (!inheritAs(prepared.nullDevice.get(), STDIN_FILENO)
            || !inheritAs(prepared.nullDevice.get(), STDOUT_FILENO)
            || !inheritAs(prepared.childStandardError(), STDERR_FILENO))
            ::_exit(ExecFailed);
        auto child = posix::FirstChildDescriptor;
        for (auto const& parked: prepared.parked)
            if (!inheritAs(parked.get(), child++))
                ::_exit(ExecFailed);
        ::closefrom(prepared.childEnd());
        if (::chdir("/") == -1)
            ::_exit(ExecFailed);
#ifdef PROC_WXMAP_CTL
        // The address space exec builds maps nothing both writable and executable. What this launcher starts
        // is an extension host on FreeBSD, whose engine interprets (it runs no compiling one there): it makes
        // no memory executable, and the host checks that this holds (LockdownStep::WriteXorExecute). Asked of
        // the next exec only: the call has no effect on this process, and its failure is the host's to state.
        auto writeXorExecute = PROC_WX_MAPPINGS_DISALLOW_EXEC;
        std::ignore = ::procctl(P_PID, 0, PROC_WXMAP_CTL, &writeXorExecute);
#endif
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
