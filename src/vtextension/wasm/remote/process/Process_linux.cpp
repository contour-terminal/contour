// SPDX-License-Identifier: Apache-2.0
//
// The launcher on Linux. posix_spawn(), whose glibc implementation clones with CLONE_VM | CLONE_VFORK rather
// than forking: no copy of a large, sanitized, multi-threaded parent, and no window in which a forked child
// may only call async-signal-safe functions. The child's descriptors are set by file actions -- the null
// device on 0 and 1, the parked inherited descriptors dup2()ed onto 3, 4, …, everything from the first beyond
// them closed -- and it starts in "/" with default signal dispositions and an empty mask.
//
// Right after the spawn the child is pinned by a pidfd: kill() signals the pidfd, tryReap() waits on it, and
// the wait set polls it, so that no number is ever used after the process behind it could have been reaped
// and the number reused. A kernel without pidfds (before 5.3) is refused.
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <csignal>
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

        [[nodiscard]] std::optional<ExitStatus> tryReap() noexcept override
        {
            if (_status)
                return _status;
            auto info = siginfo_t {};
            while (::waitid(PidfdIdType, static_cast<id_t>(_pidfd.get()), &info, WEXITED | WNOHANG) == -1)
            {
                if (errno == EINTR)
                    continue;
                // ECHILD: reaped by someone else (a SIGCHLD handler), and its status lost with it. It is gone
                // either way; the code says the status is not known.
                _status = ExitStatus { .kind = ExitKind::Exited, .value = -1 };
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
        [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
            LaunchSpec const& spec) override
        {
            auto prepared = posix::prepareLaunch(spec);
            if (!prepared)
                return std::unexpected(prepared.error());

            auto actions = posix::FileActions {};
            actions.check(
                ::posix_spawn_file_actions_adddup2(actions.get(), prepared->nullDevice.get(), STDIN_FILENO));
            actions.check(
                ::posix_spawn_file_actions_adddup2(actions.get(), prepared->nullDevice.get(), STDOUT_FILENO));
            if (prepared->standardError.get() != -1)
                actions.check(::posix_spawn_file_actions_adddup2(
                    actions.get(), prepared->standardError.get(), STDERR_FILENO));
            auto child = posix::FirstChildDescriptor;
            for (auto const& parked: prepared->parked)
                actions.check(::posix_spawn_file_actions_adddup2(actions.get(), parked.get(), child++));
            actions.check(::posix_spawn_file_actions_addclosefrom_np(actions.get(), prepared->childEnd()));
            actions.check(::posix_spawn_file_actions_addchdir_np(actions.get(), "/"));

            auto attributes = posix::SpawnAttributes {};
            auto defaults = sigset_t {};
            auto mask = sigset_t {};
            ::sigfillset(&defaults);
            ::sigemptyset(&mask);
            attributes.check(::posix_spawnattr_setsigdefault(attributes.get(), &defaults));
            attributes.check(::posix_spawnattr_setsigmask(attributes.get(), &mask));
            attributes.check(
                ::posix_spawnattr_setflags(attributes.get(), POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));
            if (actions.error() != 0)
                return std::unexpected(posix::launchErrorOf(actions.error()));
            if (attributes.error() != 0)
                return std::unexpected(posix::launchErrorOf(attributes.error()));

            auto argv = prepared->argv();
            auto envp = prepared->envp();
            auto pid = pid_t { -1 };
            if (auto const error = ::posix_spawn(&pid,
                                                 prepared->executable.c_str(),
                                                 actions.get(),
                                                 attributes.get(),
                                                 argv.data(),
                                                 envp.data());
                error != 0)
                return std::unexpected(posix::launchErrorOf(error));

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
    };
} // namespace

std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher()
{
    return std::make_shared<LinuxProcessLauncher>();
}

std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment()
{
    return {};
}

} // namespace vtextension::wasm::remote::process
