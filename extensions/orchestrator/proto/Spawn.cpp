// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Spawn.hpp>

#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>

namespace orchestrator::proto
{

namespace
{
    /// How much one read() takes.
    constexpr auto ReadChunk = std::size_t { 64 } * 1024;

    /// How long runToCompletion() sleeps at most while its child closed its output but has not exited.
    constexpr auto ReapPollMs = 20;

    /// A pipe whose both ends close on exec: the child keeps only the end dup2'd onto its stream.
    [[nodiscard]] std::expected<std::array<Descriptor, 2>, int> makePipe()
    {
        auto fds = std::array<int, 2> {};
        if (::pipe2(fds.data(), O_CLOEXEC) != 0)
            return std::unexpected(errno);
        return std::array<Descriptor, 2> { Descriptor { fds[0] }, Descriptor { fds[1] } };
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

    /// posix_spawn's two attribute objects, destroyed with this value.
    class SpawnSetup
    {
      public:
        SpawnSetup() noexcept
        {
            ::posix_spawn_file_actions_init(&_actions);
            ::posix_spawnattr_init(&_attributes);
        }
        SpawnSetup(SpawnSetup const&) = delete;
        SpawnSetup& operator=(SpawnSetup const&) = delete;
        SpawnSetup(SpawnSetup&&) = delete;
        SpawnSetup& operator=(SpawnSetup&&) = delete;
        ~SpawnSetup()
        {
            ::posix_spawnattr_destroy(&_attributes);
            ::posix_spawn_file_actions_destroy(&_actions);
        }

        [[nodiscard]] posix_spawn_file_actions_t* actions() noexcept { return &_actions; }
        [[nodiscard]] posix_spawnattr_t* attributes() noexcept { return &_attributes; }

      private:
        posix_spawn_file_actions_t _actions {};
        posix_spawnattr_t _attributes {};
    };

    /// Prepares the child: its streams, its directory, its own process group and clean signal state.
    /// @return 0, or the first error.
    [[nodiscard]] int prepare(SpawnSetup& setup, SpawnRequest const& request, int outFd, int errFd)
    {
        auto none = sigset_t {};
        sigemptyset(&none);
        auto catchable = sigset_t {};
        sigfillset(&catchable);
        sigdelset(&catchable, SIGKILL);
        sigdelset(&catchable, SIGSTOP);
#ifdef __APPLE__
        // Only the descriptors the file actions name are inherited.
        auto constexpr Flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP
                               | POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
        auto constexpr Flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
#endif
        for (auto const error: {
                 ::posix_spawn_file_actions_addopen(setup.actions(), STDIN_FILENO, "/dev/null", O_RDONLY, 0),
                 ::posix_spawn_file_actions_adddup2(setup.actions(), outFd, STDOUT_FILENO),
                 ::posix_spawn_file_actions_adddup2(setup.actions(), errFd, STDERR_FILENO),
                 ::posix_spawn_file_actions_addchdir_np(setup.actions(), request.cwd.c_str()),
                 ::posix_spawnattr_setsigmask(setup.attributes(), &none),
                 ::posix_spawnattr_setsigdefault(setup.attributes(), &catchable),
                 ::posix_spawnattr_setpgroup(setup.attributes(), 0),
                 ::posix_spawnattr_setflags(setup.attributes(), static_cast<short>(Flags)),
             })
            if (error != 0)
                return error;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
        // After the dup2s, which it would otherwise undo: nothing the driver inherited above standard error
        // reaches the child. Elsewhere the driver's own descriptors are all close-on-exec already.
        return ::posix_spawn_file_actions_addclosefrom_np(setup.actions(), STDERR_FILENO + 1);
#else
        return 0;
#endif
    }
} // namespace

Descriptor::Descriptor(Descriptor&& other) noexcept: _fd { std::exchange(other._fd, -1) }
{
}

Descriptor& Descriptor::operator=(Descriptor&& other) noexcept
{
    if (this != &other)
    {
        reset();
        _fd = std::exchange(other._fd, -1);
    }
    return *this;
}

void Descriptor::reset() noexcept
{
    if (_fd >= 0)
        ::close(std::exchange(_fd, -1));
}

Child::Child(pid_t pid, Descriptor out, Descriptor err, uint64_t deadlineMs) noexcept:
    _pid { pid }, _streams { std::move(out), std::move(err) }, _deadlineMs { deadlineMs }
{
}

std::expected<Child, StartResult> Child::spawn(SpawnRequest const& request, uint64_t nowMs)
{
    if (request.argv.empty())
        return std::unexpected { StartResult::Refused };
    auto out = makePipe();
    auto err = makePipe();
    if (!out || !err)
        return std::unexpected { StartResult::Refused };
    auto setup = SpawnSetup {};
    if (prepare(setup, request, (*out)[1].get(), (*err)[1].get()) != 0)
        return std::unexpected { StartResult::Refused };
    auto arguments = pointersTo(request.argv);
    auto environment = pointersTo(request.environment);
    auto pid = pid_t {};
    if (::posix_spawnp(&pid,
                       arguments.front(),
                       setup.actions(),
                       setup.attributes(),
                       arguments.data(),
                       environment.data())
        != 0)
        return std::unexpected { StartResult::Refused };
    auto const deadline = nowMs + static_cast<uint64_t>(request.timeout.count());
    return Child { pid, std::move((*out)[0]), std::move((*err)[0]), deadline };
}

void Child::addPollFds(std::vector<pollfd>& fds) const
{
    for (auto const& stream: _streams)
        if (stream.get() >= 0)
            fds.push_back(pollfd { .fd = stream.get(), .events = POLLIN, .revents = 0 });
}

Child::ReadResult Child::readOnce(std::size_t index)
{
    auto& stream = _streams[index];
    auto buffer = std::array<char, ReadChunk> {};
    auto const got = ::read(stream.get(), buffer.data(), buffer.size());
    if (got < 0 && (errno == EINTR || errno == EAGAIN))
        return ReadResult::Nothing;
    if (got <= 0)
    {
        stream.reset();
        return ReadResult::Closed;
    }
    auto& text = index == 0 ? _outcome.stdoutText : _outcome.stderrText;
    auto const room = MaxStreamBytes - std::min(MaxStreamBytes, text.size());
    auto const gotBytes = static_cast<std::size_t>(got);
    auto const kept = std::min(room, gotBytes);
    text.append(buffer.data(), kept);
    if (kept < gotBytes)
        _outcome.extent = OutputExtent::Truncated;
    return ReadResult::Data;
}

void Child::service(std::span<pollfd const> fds, uint64_t nowMs)
{
    for (auto const index: std::views::iota(std::size_t { 0 }, _streams.size()))
    {
        auto const polled = std::ranges::find(fds, _streams[index].get(), &pollfd::fd);
        if (_streams[index].get() >= 0 && polled != fds.end() && polled->revents != 0)
            std::ignore = readOnce(index);
    }
    noteExit(nowMs);
    if (_exitedAtMs
        && std::ranges::all_of(_streams, [](Descriptor const& stream) { return stream.get() < 0; }))
        _state = ChildState::Ended;
}

void Child::noteExit(uint64_t nowMs)
{
    if (_exitedAtMs)
        return;
    auto status = 0;
    auto const reaped = ::waitpid(_pid, &status, WNOHANG);
    if (reaped == 0 || (reaped < 0 && errno == EINTR))
        return;
    _exitedAtMs = nowMs;
    if (reaped == _pid && WIFEXITED(status))
        _outcome.exitCode = WEXITSTATUS(status);
}

void Child::drainAvailable()
{
    for (auto const index: std::views::iota(std::size_t { 0 }, _streams.size()))
        while (_streams[index].get() >= 0)
        {
            auto ready = pollfd { .fd = _streams[index].get(), .events = POLLIN, .revents = 0 };
            if (::poll(&ready, 1, 0) <= 0 || readOnce(index) != ReadResult::Data)
                break;
        }
}

void Child::closeStreams() noexcept
{
    for (auto& stream: _streams)
        stream.reset();
}

void Child::enforceDeadline(uint64_t nowMs)
{
    if (_state == ChildState::Ended)
        return;
    noteExit(nowMs);
    if (_exitedAtMs)
    {
        // It exited in time; a process it left behind holds its output open. What it wrote is kept, the
        // group is stopped, and the child counts as having exited when it did.
        if (nowMs < *_exitedAtMs + ExitGraceMs)
            return;
        drainAvailable();
        ::kill(-_pid, SIGKILL);
        closeStreams();
        _state = ChildState::Ended;
        return;
    }
    if (nowMs < _deadlineMs)
        return;
    terminate();
    _outcome.completion = Completion::TimedOut;
    _outcome.exitCode.reset();
}

void Child::terminate() noexcept
{
    if (_state == ChildState::Ended)
        return;
    // The whole group: a grandchild holding the pipes open would otherwise outlive the kill.
    ::kill(-_pid, SIGKILL);
    closeStreams();
    if (!_exitedAtMs)
    {
        auto status = 0;
        while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    _state = ChildState::Ended;
}

uint64_t Child::nextCheckMs() const noexcept
{
    return _exitedAtMs ? *_exitedAtMs + ExitGraceMs : _deadlineMs;
}

ProcessOutcome runToCompletion(SpawnRequest const& request, uint64_t nowMs)
{
    auto child = Child::spawn(request, nowMs);
    if (!child)
        return notStartedOutcome(child.error());
    auto fds = std::vector<pollfd> {};
    while (child->state() == ChildState::Collecting)
    {
        fds.clear();
        child->addPollFds(fds);
        auto const now = monotonicNowMs();
        auto const left = child->nextCheckMs() > now ? child->nextCheckMs() - now : 0;
        auto const waitMs = fds.empty() ? std::min<uint64_t>(left, ReapPollMs) : left;
        if (::poll(fds.data(), fds.size(), static_cast<int>(waitMs)) < 0 && errno != EINTR)
            break;
        child->service(fds, monotonicNowMs());
        child->enforceDeadline(monotonicNowMs());
    }
    return child->outcome();
}

ProcessOutcome notStartedOutcome(StartResult start)
{
    return ProcessOutcome { .start = start,
                            .completion = Completion::Exited,
                            .exitCode = std::nullopt,
                            .extent = OutputExtent::Whole,
                            .stdoutText = {},
                            .stderrText = {} };
}

uint64_t monotonicNowMs() noexcept
{
    auto const since = std::chrono::steady_clock::now().time_since_epoch();
    // CLOCK_MONOTONIC counts from boot, so it is never 0; the max() keeps the engine's contract regardless.
    return std::max<uint64_t>(
        1, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()));
}

} // namespace orchestrator::proto
