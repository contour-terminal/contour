// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/PosixLaunch.hpp>

#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>

#include <vtextension/wasm/BuildInstrumentation.hpp>

#ifdef __APPLE__
    #include <Availability.h>
#endif

namespace vtextension::wasm::remote::process::posix
{

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept
{
    if (this != &other)
    {
        if (_descriptor != -1)
            ::close(_descriptor);
        _descriptor = std::exchange(other._descriptor, -1);
    }
    return *this;
}

UniqueFd::~UniqueFd()
{
    if (_descriptor != -1)
        ::close(_descriptor);
}

namespace
{
    /// @return Pointers to @p strings' characters, null-terminated, as exec takes them.
    [[nodiscard]] std::vector<char*> pointersInto(std::vector<std::string>& strings)
    {
        auto pointers = std::vector<char*> {};
        pointers.reserve(strings.size() + 1);
        for (auto& text: strings)
            pointers.push_back(text.data());
        pointers.push_back(nullptr);
        return pointers;
    }
} // namespace

std::vector<char*> PreparedLaunch::argv()
{
    return pointersInto(arguments);
}

std::vector<char*> PreparedLaunch::envp()
{
    return pointersInto(environment);
}

std::expected<PreparedLaunch, LaunchError> prepareLaunch(LaunchSpec const& spec)
{
    auto prepared = PreparedLaunch { .executable = spec.executable.string(),
                                     .arguments = {},
                                     .environment = {},
                                     .parked = {},
                                     .nullDevice = {},
                                     .standardError = {} };
    // posix_spawn() reports a failed exec too, but pdfork()'s child can only exit: the check up front gives
    // every platform the same NotFound.
    if (::access(prepared.executable.c_str(), X_OK) == -1)
        return std::unexpected(LaunchError::NotFound);

    // Parked at or above the first descriptor the child does not inherit, so that the child's dup2() onto 3,
    // 4, … overwrites none of the descriptors still to be moved.
    auto const childEnd = FirstChildDescriptor + static_cast<int>(spec.inherited.size());
    for (auto const descriptor: spec.inherited)
    {
        auto const parked = ::fcntl(descriptor, F_DUPFD_CLOEXEC, childEnd);
        if (parked == -1)
            return std::unexpected(launchErrorOf(errno));
        prepared.parked.emplace_back(parked);
    }
    if (spec.standardError)
    {
        prepared.standardError = UniqueFd { ::fcntl(*spec.standardError, F_DUPFD_CLOEXEC, childEnd) };
        if (prepared.standardError.get() == -1)
            return std::unexpected(launchErrorOf(errno));
    }
    prepared.nullDevice = UniqueFd { ::open("/dev/null", O_RDWR | O_CLOEXEC) };
    if (prepared.nullDevice.get() == -1)
        return std::unexpected(launchErrorOf(errno));

    auto childHandles = std::vector<transport::NativeHandle> {};
    for (auto const index: std::views::iota(FirstChildDescriptor, childEnd))
        childHandles.push_back(index);
    prepared.arguments.push_back(prepared.executable);
    if (spec.arguments)
        std::ranges::move(spec.arguments(childHandles), std::back_inserter(prepared.arguments));
    for (auto const& [name, value]: spec.environment)
        prepared.environment.push_back(std::format("{}={}", name, value));
    return prepared;
}

LaunchError launchErrorOf(int error) noexcept
{
    switch (error)
    {
        case ENOENT:
        case EACCES:
        case ENOEXEC:
        case ENOTDIR: return LaunchError::NotFound;
        case ENOMEM:
        case EAGAIN:
        case EMFILE:
        case ENFILE: return LaunchError::Resources;
        case ENOSYS: return LaunchError::Unsupported;
        default: return LaunchError::Io;
    }
}

ExitStatus exitStatusOfWait(int status) noexcept
{
    if (WIFSIGNALED(status))
        return ExitStatus { .kind = ExitKind::Signalled, .value = WTERMSIG(status) };
    return ExitStatus { .kind = ExitKind::Exited, .value = WEXITSTATUS(status) };
}

#if defined(__linux__) || defined(__APPLE__)
namespace
{
    /// Adds the change of the child's working directory to @p actions. POSIX.1-2024 named it
    /// posix_spawn_file_actions_addchdir(); macOS 26 declares that and deprecates the _np spelling, which
    /// glibc and older macOS alone have. A build for macOS 26 and later uses POSIX's, one for older systems
    /// whichever the system it runs on has.
    [[nodiscard]] int addChdir(posix_spawn_file_actions_t* actions, char const* path) noexcept
    {
    #if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
        return ::posix_spawn_file_actions_addchdir(actions, path);
    #elif defined(__APPLE__) && defined(__MAC_26_0)
        if (__builtin_available(macOS 26.0, *))
            return ::posix_spawn_file_actions_addchdir(actions, path);
        return ::posix_spawn_file_actions_addchdir_np(actions, path);
    #else
        return ::posix_spawn_file_actions_addchdir_np(actions, path);
    #endif
    }
} // namespace

std::expected<pid_t, LaunchError> spawn(PreparedLaunch& prepared)
{
    auto actions = FileActions {};
    actions.check(::posix_spawn_file_actions_adddup2(actions.get(), prepared.nullDevice.get(), STDIN_FILENO));
    actions.check(
        ::posix_spawn_file_actions_adddup2(actions.get(), prepared.nullDevice.get(), STDOUT_FILENO));
    actions.check(
        ::posix_spawn_file_actions_adddup2(actions.get(), prepared.childStandardError(), STDERR_FILENO));
    auto child = FirstChildDescriptor;
    for (auto const& parked: prepared.parked)
        actions.check(::posix_spawn_file_actions_adddup2(actions.get(), parked.get(), child++));
    #ifdef __APPLE__
    // POSIX_SPAWN_CLOEXEC_DEFAULT closes every descriptor no action names.
    constexpr auto ClosingFlags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    #else
    actions.check(::posix_spawn_file_actions_addclosefrom_np(actions.get(), prepared.childEnd()));
    constexpr auto ClosingFlags = 0;
    #endif
    actions.check(addChdir(actions.get(), "/"));

    auto attributes = SpawnAttributes {};
    auto defaults = sigset_t {};
    auto mask = sigset_t {};
    sigfillset(&defaults);
    sigemptyset(&mask);
    attributes.check(::posix_spawnattr_setsigdefault(attributes.get(), &defaults));
    attributes.check(::posix_spawnattr_setsigmask(attributes.get(), &mask));
    attributes.check(
        ::posix_spawnattr_setflags(attributes.get(),
                                   static_cast<short>(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK
                                                      | POSIX_SPAWN_SETSID | ClosingFlags)));
    if (actions.error() != 0)
        return std::unexpected(launchErrorOf(actions.error()));
    if (attributes.error() != 0)
        return std::unexpected(launchErrorOf(attributes.error()));

    auto argv = prepared.argv();
    auto envp = prepared.envp();
    auto pid = pid_t { -1 };
    if (auto const error = ::posix_spawn(
            &pid, prepared.executable.c_str(), actions.get(), attributes.get(), argv.data(), envp.data());
        error != 0)
        return std::unexpected(launchErrorOf(error));
    return pid;
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
std::expected<std::optional<ExitStatus>, LaunchError> reapChild(pid_t pid) noexcept
{
    auto status = 0;
    while (true)
    {
        auto const reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == 0)
            return std::nullopt;
        if (reaped == pid)
            return exitStatusOfWait(status);
        if (errno == EINTR)
            continue;
        if (errno == ECHILD)
            return LostExitStatus;
        return std::unexpected(launchErrorOf(errno));
    }
}
#endif

#ifndef __APPLE__
namespace
{
    /// Polls process descriptors and a wake pipe.
    class PollExitWaitSet final: public IExitWaitSet
    {
      public:
        PollExitWaitSet(core::IClock& clock, UniqueFd wakeRead, UniqueFd wakeWrite) noexcept:
            _clock { &clock }, _wakeRead { std::move(wakeRead) }, _wakeWrite { std::move(wakeWrite) }
        {
        }

        [[nodiscard]] std::expected<void, LaunchError> watch(uint64_t key,
                                                             IProcessHandle const& process) override
        {
            _watched.insert_or_assign(key, process.waitable());
            return {};
        }

        void unwatch(uint64_t key) noexcept override { _watched.erase(key); }

        [[nodiscard]] std::expected<std::vector<uint64_t>, LaunchError> wait(
            std::optional<core::SteadyTimePoint> deadline) override
        {
            auto descriptors = std::vector<pollfd> {};
            descriptors.reserve(_watched.size() + 1);
            descriptors.push_back(pollfd { .fd = _wakeRead.get(), .events = POLLIN, .revents = 0 });
            for (auto const& [key, descriptor]: _watched)
                descriptors.push_back(pollfd { .fd = descriptor, .events = POLLIN, .revents = 0 });

            while (true)
            {
                auto const ready = ::poll(descriptors.data(), descriptors.size(), timeoutOf(deadline));
                if (ready == -1 && errno == EINTR)
                    continue;
                if (ready == -1)
                    return std::unexpected(launchErrorOf(errno));
                if (ready == 0)
                    return std::vector<uint64_t> {};
                break;
            }
            // A descriptor closed while watched: its number may name anything by now.
            if (std::ranges::any_of(descriptors,
                                    [](pollfd const& polled) { return (polled.revents & POLLNVAL) != 0; }))
                return std::unexpected(LaunchError::Io);
            if (descriptors.front().revents != 0)
                drainWakes();
            // A process descriptor polls readable (Linux) or hung up (FreeBSD) once its process exited.
            auto ended = std::vector<uint64_t> {};
            for (auto const& [watch, descriptor]:
                 std::views::zip(_watched, std::span { descriptors }.subspan(1)))
                if (descriptor.revents != 0)
                    ended.push_back(watch.first);
            return ended;
        }

        void wake() noexcept override
        {
            auto const wake = uint8_t { 1 };
            // A full pipe holds a wake already.
            while (::write(_wakeWrite.get(), &wake, sizeof(wake)) == -1 && errno == EINTR)
            {
            }
        }

      private:
        /// @return poll()'s timeout until @p deadline: -1 for none, never less than 0, rounded up to the next
        ///         millisecond so that a wait never ends before its deadline.
        [[nodiscard]] int timeoutOf(std::optional<core::SteadyTimePoint> deadline) const noexcept
        {
            if (!deadline)
                return -1;
            auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - _clock->now());
            constexpr auto Longest = std::chrono::milliseconds { std::numeric_limits<int>::max() };
            return static_cast<int>(std::clamp(remaining, std::chrono::milliseconds { 0 }, Longest).count());
        }

        /// Takes every byte queued on the wake pipe.
        void drainWakes() noexcept
        {
            auto buffer = std::array<uint8_t, 64> {};
            while (true)
            {
                auto const received = ::read(_wakeRead.get(), buffer.data(), buffer.size());
                if (received > 0 || (received == -1 && errno == EINTR))
                    continue;
                return;
            }
        }

        core::IClock* _clock;
        UniqueFd _wakeRead;
        UniqueFd _wakeWrite;
        std::map<uint64_t, int> _watched; ///< Each key's process descriptor, owned by its handle.
    };
} // namespace

std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makePollExitWaitSet(core::IClock& clock)
{
    auto ends = std::array<int, 2> { -1, -1 };
    if (::pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) == -1)
        return std::unexpected(launchErrorOf(errno));
    return std::make_unique<PollExitWaitSet>(clock, UniqueFd { ends[0] }, UniqueFd { ends[1] });
}
#endif

} // namespace vtextension::wasm::remote::process::posix

namespace vtextension::wasm::remote::process
{

ProcessId currentProcessId() noexcept
{
    return static_cast<ProcessId>(::getpid());
}

std::optional<transport::NativeHandle> defaultHelperStandardError()
{
    if constexpr (ThisBuildInstrumentation != BuildInstrumentation::None)
        return STDERR_FILENO;
    return std::nullopt;
}

} // namespace vtextension::wasm::remote::process
