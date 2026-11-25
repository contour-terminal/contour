// SPDX-License-Identifier: Apache-2.0
//
// The launcher on Linux. posix_spawn(), whose glibc implementation clones with CLONE_VM | CLONE_VFORK rather
// than forking: no copy of a large, sanitized, multi-threaded parent, and no window in which a forked child
// may only call async-signal-safe functions. What the child starts with is PosixLaunch's spawn(): the null
// device on 0 and 1, its stderr, the parked inherited descriptors dup2()ed onto 3, 4, …, everything from
// the first beyond them closed, "/" as its working directory, a session of its own, default signal
// dispositions and an empty mask.
//
// Right after the spawn the child is pinned by a pidfd: kill() signals the pidfd, tryReap() waits on it, and
// the wait set polls it, so that no number is ever used after the process behind it could have been reaped
// and the number reused. A kernel that cannot wait on a pidfd (before 5.4; pidfds themselves came in 5.3)
// is found out once, by makeSystemProcessLauncher(), and the launcher it makes refuses every launch as
// Unsupported.
#include <vtextension/wasm/remote/process/Process_linux.hpp>

#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <vtextension/wasm/remote/process/PosixLaunch.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

namespace
{
    using posix::UniqueFd;

    /// waitid()'s id type for a pidfd: an enumerator of glibc's, not a macro, and not in older headers.
    constexpr auto PidfdIdType = static_cast<idtype_t>(3);

    /// @return A pidfd for @p pid, close-on-exec; or the errno pidfd_open() failed with.
    [[nodiscard]] std::expected<UniqueFd, int> openPidfd(pid_t pid) noexcept
    {
        auto const pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
        if (pidfd == -1)
            return std::unexpected(errno);
        return UniqueFd { pidfd };
    }

    class LinuxProcessHandle final: public IProcessHandle
    {
      public:
        LinuxProcessHandle(pid_t pid, UniqueFd pidfd) noexcept: _pid { pid }, _pidfd { std::move(pidfd) } {}

        [[nodiscard]] ProcessId id() const noexcept override { return _pid; }

        void kill() noexcept override
        {
            if (!_status)
                ::syscall(SYS_pidfd_send_signal, _pidfd.get(), SIGKILL, nullptr, 0);
        }

        [[nodiscard]] std::expected<std::optional<ExitStatus>, LaunchError> tryReap() noexcept override
        {
            if (_status)
                return _status;
            auto info = siginfo_t {};
            while (::waitid(PidfdIdType, static_cast<id_t>(_pidfd.get()), &info, WEXITED | WNOHANG) == -1)
            {
                if (errno == EINTR)
                    continue;
                // ECHILD: reaped by someone else (a SIGCHLD handler), and its status lost with it. It is gone
                // either way; the code says the status is not known. Any other error leaves it unreaped.
                if (errno != ECHILD)
                    return std::unexpected(posix::launchErrorOf(errno));
                _status = LostExitStatus;
                return _status;
            }
            if (info.si_pid == 0)
                return std::nullopt;
            _status = info.si_code == CLD_EXITED
                          ? ExitStatus { .kind = ExitKind::Exited, .value = info.si_status }
                          : ExitStatus { .kind = ExitKind::Signalled, .value = info.si_status };
            return _status;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return _pidfd.get(); }

      private:
        pid_t _pid;
        UniqueFd _pidfd;
        std::optional<ExitStatus> _status; ///< Set once reaped.
    };

    class LinuxProcessLauncher final: public IProcessLauncher
    {
      public:
        explicit LinuxProcessLauncher(PidfdWaits pidfdWaits) noexcept: _pidfdWaits { pidfdWaits } {}

        [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
            LaunchSpec const& spec) override
        {
            // Refused before anything starts: a child that could not be waited for must not run.
            if (_pidfdWaits == PidfdWaits::Unsupported)
                return std::unexpected(LaunchError::Unsupported);
            auto prepared = posix::prepareLaunch(spec);
            if (!prepared)
                return std::unexpected(prepared.error());
            auto const spawned = posix::spawn(*prepared);
            if (!spawned)
                return std::unexpected(spawned.error());
            auto const pid = *spawned;

            auto pidfd = openPidfd(pid);
            if (!pidfd)
            {
                // Not to be watched safely: it must not run unwatched either. Not yet reaped, the pid is
                // still its own.
                ::kill(pid, SIGKILL);
                while (::waitpid(pid, nullptr, 0) == -1 && errno == EINTR)
                {
                }
                return std::unexpected(posix::launchErrorOf(pidfd.error()));
            }
            return std::make_unique<LinuxProcessHandle>(pid, *std::move(pidfd));
        }

        [[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
            core::IClock& clock) override
        {
            return posix::makePollExitWaitSet(clock);
        }

      private:
        PidfdWaits _pidfdWaits; ///< Whether a child can be waited for at all.
    };
} // namespace

PidfdWaits probePidfdWaits() noexcept
{
    auto const own = openPidfd(::getpid());
    if (!own)
        return own.error() == ENOSYS ? PidfdWaits::Unsupported : PidfdWaits::Supported;
    auto info = siginfo_t {};
    auto const waited = ::waitid(PidfdIdType, static_cast<id_t>(own->get()), &info, WEXITED | WNOHANG);
    return waited == -1 && errno == EINVAL ? PidfdWaits::Unsupported : PidfdWaits::Supported;
}

std::shared_ptr<IProcessLauncher> makeLinuxProcessLauncher(PidfdWaits pidfdWaits)
{
    return std::make_shared<LinuxProcessLauncher>(pidfdWaits);
}

std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher()
{
    return makeLinuxProcessLauncher(probePidfdWaits());
}

std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment()
{
    return {};
}

} // namespace vtextension::wasm::remote::process
