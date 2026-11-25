// SPDX-License-Identifier: Apache-2.0
//
// The launcher on macOS. posix_spawn() with POSIX_SPAWN_CLOEXEC_DEFAULT, Apple's extension that closes every
// descriptor no file action names: the null device on 0 and 1, stderr inherited, the parked inherited
// descriptors dup2()ed onto 3, 4, …; the child starts in "/" with default signal dispositions and an empty
// mask.
//
// The parent's own ends are close-on-exec, but between their creation and that flag (macOS has no
// SOCK_CLOEXEC) a concurrent fork() and exec() elsewhere in the process could inherit one; the spawn itself
// cannot, as CLOEXEC_DEFAULT closes all it did not name.
//
// macOS has no process descriptors: a process is its pid, watched through kqueue's EVFILT_PROC. kill() is
// guarded by the handle's reaped status, as a pid once reaped may name another process.
#include <sys/event.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spawn.h>
#include <unistd.h>

#include <vtextension/wasm/remote/process/PosixLaunch.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

namespace
{
    using posix::UniqueFd;

    /// kqueue's event record, named apart from the function of the same name.
    using KernelEvent = struct kevent;

    /// The EVFILT_USER event a wake triggers.
    constexpr auto WakeIdent = uintptr_t { 0 };

    class MacProcessHandle final: public IProcessHandle
    {
      public:
        explicit MacProcessHandle(pid_t pid) noexcept: _pid { pid } {}

        [[nodiscard]] ProcessId id() const noexcept override { return _pid; }

        void kill() noexcept override
        {
            if (!_status)
                ::kill(_pid, SIGKILL);
        }

        [[nodiscard]] std::optional<ExitStatus> tryReap() noexcept override
        {
            if (_status)
                return _status;
            auto status = 0;
            while (true)
            {
                auto const reaped = ::waitpid(_pid, &status, WNOHANG);
                if (reaped == 0)
                    return std::nullopt;
                if (reaped == _pid)
                    break;
                if (errno == EINTR)
                    continue;
                // ECHILD: reaped by someone else, and its status lost with it.
                _status = ExitStatus { .kind = ExitKind::Exited, .value = -1 };
                return _status;
            }
            _status = posix::exitStatusOfWait(status);
            return _status;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return _pid; }

      private:
        pid_t _pid;
        std::optional<ExitStatus> _status; ///< Set once reaped.
    };

    /// Waits on a kqueue: EVFILT_PROC NOTE_EXIT per process, EVFILT_USER for wake().
    class KqueueExitWaitSet final: public IExitWaitSet
    {
      public:
        KqueueExitWaitSet(core::IClock& clock, UniqueFd queue) noexcept:
            _clock { &clock }, _queue { std::move(queue) }
        {
        }

        [[nodiscard]] std::expected<void, LaunchError> watch(uint64_t key,
                                                             IProcessHandle const& process) override
        {
            auto const pid = process.waitable();
            auto change = KernelEvent {};
            EV_SET(&change,
                   static_cast<uintptr_t>(pid),
                   EVFILT_PROC,
                   EV_ADD | EV_ONESHOT,
                   NOTE_EXIT,
                   0,
                   reinterpret_cast<void*>(static_cast<uintptr_t>(key)));
            if (::kevent(_queue.get(), &change, 1, nullptr, 0, nullptr) == -1)
            {
                // ESRCH: it exited before it could be watched; it is reported at the next wait.
                if (errno != ESRCH)
                    return std::unexpected(posix::launchErrorOf(errno));
                _exited.push_back(key);
            }
            _watched.insert_or_assign(key, pid);
            return {};
        }

        void unwatch(uint64_t key) noexcept override
        {
            auto const found = _watched.find(key);
            if (found == _watched.end())
                return;
            auto change = KernelEvent {};
            EV_SET(&change, static_cast<uintptr_t>(found->second), EVFILT_PROC, EV_DELETE, 0, 0, nullptr);
            // Fails harmlessly once the one-shot event fired.
            ::kevent(_queue.get(), &change, 1, nullptr, 0, nullptr);
            _watched.erase(found);
            std::erase(_exited, key);
        }

        [[nodiscard]] std::vector<uint64_t> wait(std::optional<core::SteadyTimePoint> deadline) override
        {
            // An exit seen once is reported as long as it is watched, as a polled descriptor would be.
            auto ended = _exited;
            auto timeout = timespec {};
            auto const hasTimeout = !ended.empty() || deadline.has_value();
            if (ended.empty() && deadline)
            {
                auto const remaining = std::max(core::SteadyDuration::zero(), *deadline - _clock->now());
                auto const seconds = std::chrono::floor<std::chrono::seconds>(remaining);
                timeout.tv_sec = static_cast<time_t>(seconds.count());
                timeout.tv_nsec = static_cast<long>(
                    std::chrono::ceil<std::chrono::nanoseconds>(remaining - seconds).count());
            }
            auto events = std::array<KernelEvent, 16> {};
            auto received = 0;
            while (true)
            {
                received = ::kevent(_queue.get(),
                                    nullptr,
                                    0,
                                    events.data(),
                                    static_cast<int>(events.size()),
                                    hasTimeout ? &timeout : nullptr);
                if (received != -1 || errno != EINTR)
                    break;
            }
            for (auto const& event:
                 std::span { events }.first(static_cast<std::size_t>(std::max(received, 0))))
            {
                if (event.filter != EVFILT_PROC)
                    continue;
                auto const key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(event.udata));
                if (_watched.contains(key) && std::ranges::find(_exited, key) == _exited.end())
                {
                    _exited.push_back(key);
                    ended.push_back(key);
                }
            }
            return ended;
        }

        void wake() noexcept override
        {
            auto change = KernelEvent {};
            EV_SET(&change, WakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            ::kevent(_queue.get(), &change, 1, nullptr, 0, nullptr);
        }

      private:
        core::IClock* _clock;
        UniqueFd _queue;
        std::map<uint64_t, pid_t> _watched;
        std::vector<uint64_t> _exited; ///< Keys whose exit was seen: reported until unwatched.
    };

    class MacProcessLauncher final: public IProcessLauncher
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
            actions.check(::posix_spawn_file_actions_addinherit_np(actions.get(), STDERR_FILENO));
            auto child = posix::FirstChildDescriptor;
            for (auto const& parked: prepared->parked)
                actions.check(::posix_spawn_file_actions_adddup2(actions.get(), parked.get(), child++));
            actions.check(::posix_spawn_file_actions_addchdir_np(actions.get(), "/"));

            auto attributes = posix::SpawnAttributes {};
            auto defaults = sigset_t {};
            auto mask = sigset_t {};
            sigfillset(&defaults);
            sigemptyset(&mask);
            attributes.check(::posix_spawnattr_setsigdefault(attributes.get(), &defaults));
            attributes.check(::posix_spawnattr_setsigmask(attributes.get(), &mask));
            attributes.check(
                ::posix_spawnattr_setflags(attributes.get(),
                                           static_cast<short>(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK
                                                              | POSIX_SPAWN_CLOEXEC_DEFAULT)));
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
            return std::make_unique<MacProcessHandle>(pid);
        }

        [[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
            core::IClock& clock) override
        {
            // kqueue descriptors are not inherited by children: no close-on-exec needed.
            auto queue = UniqueFd { ::kqueue() };
            if (queue.get() == -1)
                return std::unexpected(posix::launchErrorOf(errno));
            struct kevent change {};
            EV_SET(&change, WakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
            if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) == -1)
                return std::unexpected(posix::launchErrorOf(errno));
            return std::make_unique<KqueueExitWaitSet>(clock, std::move(queue));
        }
    };
} // namespace

std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher()
{
    return std::make_shared<MacProcessLauncher>();
}

std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment()
{
    return {};
}

} // namespace vtextension::wasm::remote::process
