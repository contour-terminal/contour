// SPDX-License-Identifier: Apache-2.0
#include <vtpty/OwnedFd.hpp>
#include <vtpty/PosixSpawn.hpp>
#include <vtpty/ProcessRunner.hpp>
#include <vtpty/ProgramLookup.hpp>
#include <vtpty/UnixUtils.hpp>

#include <sys/stat.h>
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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>

#ifdef __linux__
    #include <sys/syscall.h>
#endif

namespace vtpty
{

namespace
{
    using core::platform::SteadyTimePoint;

    /// How much is read from one pipe in one go.
    constexpr auto ReadChunkSize = std::size_t { 64 } * 1024;

    /// How many chunks one pipe may give in one round, so that a process writing without pause cannot keep
    /// the runner from its deadlines and its other processes.
    constexpr auto ReadChunksPerRound = 16;

    /// How often a leader is looked for while there is no process descriptor that wakes the runner when it
    /// exits: on kernels and platforms without pidfd_open().
    constexpr auto ReapSlice = std::chrono::milliseconds { 20 };

    /// How long the destructor waits, in slices, for the killed members of a group to be reaped by
    /// whoever inherited them. Bounded: a grandchild stuck in an uninterruptible sleep is left behind
    /// rather than keeping the destructor (and with it the daemon's shutdown) from returning.
    constexpr auto GroupGoneSlices = 100;
    constexpr auto GroupGoneSlice = std::chrono::milliseconds { 10 };

    /// Where a process is in being ended by the runner.
    enum class Ending : uint8_t
    {
        NotAsked,    ///< Nobody has asked it to end.
        Terminating, ///< SIGTERM went to its group; SIGKILL follows at killAt, whatever the leader does.
        Killed,      ///< SIGKILL went to its group.
    };

    /// Where a process's leader is. It is reaped only when the process completes, so that until then its id
    /// -- the id of its group -- stays taken, and a signal to the group reaches no unrelated process.
    enum class Leader : uint8_t
    {
        Running, ///< It has not exited.
        Exited,  ///< It has exited and is not reaped: a zombie that holds its id.
        Reaped,  ///< It has been reaped; its id is free.
    };

    /// One of a process's two output pipes, and what was read from it.
    struct Stream
    {
        OwnedFd pipe;            ///< The read end; closed once it reported EOF (or was given up on).
        std::size_t cap = 0;     ///< How many bytes are kept; 0 to discard.
        CapturedStream captured; ///< What was kept.
    };

    /// A started process.
    struct Child
    {
        pid_t pid = 0;                            ///< The leader, and the id of its process group.
        OwnedFd exitDescriptor;                   ///< Readable once the leader exits (Linux); -1 elsewhere.
        std::array<Stream, 2> streams;            ///< Standard output, then standard error.
        SteadyTimePoint deadline;                 ///< When it is timed out.
        Ending ending = Ending::NotAsked;         ///< How far it has been asked to end.
        SteadyTimePoint killAt;                   ///< When it is killed, while Terminating.
        std::optional<ProcessCompletion> endedBy; ///< TimedOut or Cancelled, once the runner ended it.
        Leader leader = Leader::Running;          ///< Where its leader is.
        SteadyTimePoint leaderExitedAt;           ///< When the leader's exit was seen, once it was.
        std::optional<int> waitStatus;            ///< The leader's wait status, once reaped; lost if not.
        CompletionHandler done;

        /// @return Whether a pipe of it is still open.
        [[nodiscard]] bool hasOpenStream() const noexcept
        {
            return std::ranges::any_of(streams, [](Stream const& stream) { return stream.pipe.isOpen(); });
        }

        /// @return Whether its leader may be reaped now: it has exited, holds no pipe, and is not waiting
        ///         for the SIGKILL of a termination.
        [[nodiscard]] bool isReapable() const noexcept
        {
            return leader == Leader::Exited && ending != Ending::Terminating && !hasOpenStream();
        }

        /// @return Whether it has ended: its leader reaped and its pipes closed.
        [[nodiscard]] bool hasEnded() const noexcept { return leader == Leader::Reaped && !hasOpenStream(); }

        /// Sends @p signal to its whole process group.
        void signalGroup(int signal) const noexcept { ::kill(-pid, signal); }

        /// @return How it ended, for its handler.
        [[nodiscard]] ProcessExit exit() &&
        {
            auto result = ProcessExit {};
            if (waitStatus && WIFEXITED(*waitStatus))
                result.exitCode = WEXITSTATUS(*waitStatus);
            else if (waitStatus && WIFSIGNALED(*waitStatus))
                result.signal = WTERMSIG(*waitStatus);
            if (endedBy)
                result.completion = *endedBy;
            else if (!waitStatus)
                result.completion = ProcessCompletion::StatusLost;
            else
                result.completion = result.signal ? ProcessCompletion::Signalled : ProcessCompletion::Exited;
            result.standardOutput = std::move(streams[0].captured);
            result.standardError = std::move(streams[1].captured);
            return result;
        }
    };

    /// A pipe whose both ends close on exec, its read end non-blocking.
    struct Pipe
    {
        OwnedFd read;
        OwnedFd write;
    };

    [[nodiscard]] std::expected<Pipe, std::error_code> makePipe()
    {
        auto fds = std::array<int, 2> { -1, -1 };
#if defined(__linux__) || defined(__FreeBSD__)
        // Atomic: another thread's spawn between pipe() and fcntl() would leak the ends into its child.
        if (::pipe2(fds.data(), O_CLOEXEC) != 0)
            return std::unexpected(util::lastError());
        auto result = Pipe { .read = OwnedFd { fds[0] }, .write = OwnedFd { fds[1] } };
#else
        // macOS has no pipe2(); its children start with POSIX_SPAWN_CLOEXEC_DEFAULT, which covers the gap.
        if (::pipe(fds.data()) != 0)
            return std::unexpected(util::lastError());
        auto result = Pipe { .read = OwnedFd { fds[0] }, .write = OwnedFd { fds[1] } };
        for (auto const fd: fds)
            if (auto const closing = util::setCloseOnExec(fd); !closing)
                return std::unexpected(closing.error());
#endif
        if (auto const nonBlocking = util::setNonBlocking(result.read.get()); !nonBlocking)
            return std::unexpected(nonBlocking.error());
        return result;
    }

    /// @return A descriptor that becomes readable when the child @p pid exits; none where the platform or
    ///         kernel has no such thing.
    [[nodiscard]] OwnedFd exitDescriptorOf(pid_t pid) noexcept
    {
#if defined(__linux__) && defined(SYS_pidfd_open)
        // Close-on-exec by definition. The pid cannot be reused before it is reaped, which only we do.
        return OwnedFd { static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)) };
#else
        (void) pid;
        return OwnedFd {};
#endif
    }

    /// Whether a leader has exited, by a look that does not reap it.
    enum class LeaderLook : uint8_t
    {
        Running,
        Exited, ///< It has exited, and is a zombie of ours.
        Gone,   ///< It is no child of ours any more: reaped by someone else.
    };

    [[nodiscard]] LeaderLook lookAtLeader(pid_t pid) noexcept
    {
        auto info = siginfo_t {};
        while (true)
        {
            if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0)
                return info.si_pid == pid ? LeaderLook::Exited : LeaderLook::Running;
            if (errno != EINTR)
                return errno == ECHILD ? LeaderLook::Gone : LeaderLook::Running;
        }
    }

    /// Whether a failed non-blocking call only reports that it would have blocked.
    /// @param error The errno the call left.
    /// @return True for EAGAIN or EWOULDBLOCK. POSIX lets the two differ; on Linux they are one
    ///         value, and comparing against both trips GCC's -Wlogical-op.
    [[nodiscard]] constexpr bool wouldBlock(int error) noexcept
    {
#if EAGAIN == EWOULDBLOCK
        return error == EAGAIN;
#else
        return error == EAGAIN || error == EWOULDBLOCK;
#endif
    }

    /// Reads what @p stream's pipe has now, keeping up to its cap, and closes it at EOF or on an error.
    void readStream(Stream& stream)
    {
        auto buffer = std::array<char, ReadChunkSize> {};
        for ([[maybe_unused]] auto const chunk: std::views::iota(0, ReadChunksPerRound))
        {
            auto const count = ::read(stream.pipe.get(), buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && wouldBlock(errno))
                return;
            if (count <= 0)
            {
                stream.pipe.reset();
                return;
            }
            auto const read = static_cast<std::size_t>(count);
            auto const kept = std::min(read, stream.cap - std::min(stream.cap, stream.captured.bytes.size()));
            stream.captured.bytes.append(buffer.data(), kept);
            if (kept < read)
                stream.captured.extent = OutputExtent::Truncated;
        }
    }

    /// @return @p spec's environment as exec() takes it, "NAME=VALUE" each.
    [[nodiscard]] std::vector<std::string> environmentEntries(ProcessSpec const& spec)
    {
        auto entries = std::vector<std::string> {};
        entries.reserve(spec.environment.size());
        for (auto const& [name, value]: spec.environment)
            entries.push_back(std::format("{}={}", name, value));
        return entries;
    }

    /// @return Pointers to @p strings' characters, ended by a null pointer, as exec() takes them.
    [[nodiscard]] std::vector<char*> pointersTo(std::vector<std::string>& strings)
    {
        auto pointers = std::vector<char*> {};
        pointers.reserve(strings.size() + 1);
        for (auto& entry: strings)
            pointers.push_back(entry.data());
        pointers.push_back(nullptr);
        return pointers;
    }

    /// Finds the file @p spec's program is, on the absolute entries of the spec's own PATH
    /// (findHeldDirectoryProgram()): the directory a command runs in may hold what a program wrote there. A
    /// relative path the manifest names is left relative: the child resolves it after it entered its
    /// directory by handle, so that a directory swapped in under the same name meanwhile cannot change the
    /// program, and it is looked up here relative to that same handle.
    /// @return What to hand posix_spawn(); std::nullopt where there is nothing to run.
    [[nodiscard]] std::optional<std::string> programOf(ProcessSpec const& spec)
    {
        auto const directory = spec.workingDirectory ? spec.workingDirectory->descriptor() : -1;
        auto const path =
            std::ranges::find(spec.environment, std::string_view { "PATH" }, [](auto const& entry) {
                return std::string_view { entry.first };
            });
        return findHeldDirectoryProgram(
            spec.argv.front(),
            execSearchPath(path == spec.environment.end() ? std::nullopt
                                                          : std::optional<std::string> { path->second }),
            [directory](std::string const& candidate) { return util::isExecutableIn(directory, candidate); });
    }

    /// @return Whether @p directory's directory has been removed since it was opened.
    [[nodiscard]] bool isRemoved(DirectoryHandle const& directory) noexcept
    {
        struct stat status {};
        return ::fstat(directory.descriptor(), &status) != 0 || status.st_nlink == 0;
    }

    /// @return What a posix_spawn() result @p error means for the start.
    [[nodiscard]] ProcessStartError startError(int error) noexcept
    {
        switch (error)
        {
            case ENOENT:
            case EACCES:
            case ENOEXEC: return ProcessStartError::ProgramNotFound;
            default: return ProcessStartError::SpawnFailed;
        }
    }

    /// The child's side of the spawn: its working directory, its three standard streams, and nothing else.
    [[nodiscard]] std::expected<SpawnFileActions, ProcessStartError> fileActionsFor(
        std::optional<DirectoryHandle> const& directory, Pipe const& output, Pipe const& error)
    {
        auto actions = SpawnFileActions::create();
        if (!actions)
            return std::unexpected(ProcessStartError::SpawnFailed);
        auto results = std::vector<int> {};
        if (directory)
        {
#ifdef HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDFCHDIR_NP
            results.push_back(
                ::posix_spawn_file_actions_addfchdir_np(actions->get(), directory->descriptor()));
#else
            return std::unexpected(ProcessStartError::Unsupported);
#endif
        }
        // dup2 clears close-on-exec on the target; every other descriptor of ours closes on exec.
        results.push_back(
            ::posix_spawn_file_actions_addopen(actions->get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0));
        results.push_back(
            ::posix_spawn_file_actions_adddup2(actions->get(), output.write.get(), STDOUT_FILENO));
        results.push_back(
            ::posix_spawn_file_actions_adddup2(actions->get(), error.write.get(), STDERR_FILENO));
#ifdef HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
        // Last: the directory and the pipes above are descriptors of 3 and up. Closes what another thread
        // opened without close-on-exec, too.
        results.push_back(::posix_spawn_file_actions_addclosefrom_np(actions->get(), 3));
#endif
        if (std::ranges::any_of(results, [](int result) { return result != 0; }))
            return std::unexpected(ProcessStartError::SpawnFailed);
        return std::move(*actions);
    }

    /// The flags that make the child the leader of a session of its own, with no controlling terminal: an
    /// open of /dev/tty fails (ENXIO) where in a background group of ours it would stop the child on its
    /// first read. Where there is no such flag, a process group of its own. Either way its group's id is its
    /// own, so that ending it reaches what it started.
    constexpr auto OwnGroupFlags =
#ifdef POSIX_SPAWN_SETSID
        POSIX_SPAWN_SETSID
#else
        POSIX_SPAWN_SETPGROUP
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        // macOS: nothing but the file actions' descriptors reaches the child.
        | POSIX_SPAWN_CLOEXEC_DEFAULT
#endif
        ;

    /// The child's attributes: a session (or process group) of its own, with no signal blocked or ignored.
    [[nodiscard]] std::expected<SpawnAttributes, ProcessStartError> attributesForChild()
    {
        auto attributes = SpawnAttributes::create();
        if (!attributes || !resetSignals(*attributes))
            return std::unexpected(ProcessStartError::SpawnFailed);
        auto flags = short {};
        if (::posix_spawnattr_getflags(attributes->get(), &flags) != 0
            || ::posix_spawnattr_setflags(attributes->get(), static_cast<short>(flags | OwnGroupFlags)) != 0
            || ::posix_spawnattr_setpgroup(attributes->get(), 0) != 0)
            return std::unexpected(ProcessStartError::SpawnFailed);
        return std::move(*attributes);
    }

    /// Waits, a bounded time, until no process of the groups @p groups is left, reaped by whoever
    /// inherited it.
    void awaitGroupsGone(std::vector<pid_t> groups) noexcept
    {
        for ([[maybe_unused]] auto const slice: std::views::iota(0, GroupGoneSlices))
        {
            std::erase_if(groups, [](pid_t group) { return ::kill(-group, 0) != 0 && errno == ESRCH; });
            if (groups.empty())
                return;
            ::poll(nullptr, 0, static_cast<int>(GroupGoneSlice.count()));
        }
    }

    /// Reaps @p child's leader, which has exited.
    void reapLeader(Child& child) noexcept
    {
        auto status = 0;
        auto reaped = pid_t {};
        while ((reaped = ::waitpid(child.pid, &status, 0)) < 0 && errno == EINTR)
        {
        }
        child.waitStatus = reaped == child.pid ? std::optional { status } : std::nullopt;
        child.leader = Leader::Reaped;
    }
} // namespace

struct PipedProcessRunner::State
{
    State(core::platform::IClock& clock, ProcessRunnerTuning tuning, Pipe wakePipe) noexcept:
        clock { clock },
        tuning { tuning },
        wakeRead { std::move(wakePipe.read) },
        wakeWrite { std::move(wakePipe.write) }
    {
    }

    /// Ends what is due by @p now: times out, kills after the grace, gives up on an orphan's pipes; then
    /// reaps the leaders that may be.
    void applyDeadlines(SteadyTimePoint now)
    {
        for (auto& child: children | std::views::values)
        {
            if (child.leader == Leader::Reaped)
            {
                // Reaped by someone else while a pipe is held: its group's id may be another's by now, so
                // nothing is signalled; the pipes are given up on after the orphan's grace.
                if (child.hasOpenStream() && now >= child.leaderExitedAt + tuning.orphanedOutputGrace)
                    closeStreams(child);
                continue;
            }
            if (child.ending == Ending::NotAsked && now >= child.deadline)
                terminate(child, ProcessCompletion::TimedOut, now);
            else if (child.ending == Ending::Terminating
                     && (now >= child.killAt || child.leader == Leader::Exited))
            {
                // At the end of the grace, or as soon as the leader ended on SIGTERM -- still unreaped, so
                // the group's id is still its own: what is left of the group ends now.
                child.signalGroup(SIGKILL);
                child.ending = Ending::Killed;
            }
            else if (child.ending == Ending::NotAsked && child.leader == Leader::Exited
                     && child.hasOpenStream() && now >= child.leaderExitedAt + tuning.orphanedOutputGrace)
            {
                // The leader is gone; what is left of its group holds a pipe. Ends it, keeps what it wrote.
                child.signalGroup(SIGKILL);
                closeStreams(child);
            }
            if (child.isReapable())
                reapLeader(child);
        }
    }

    /// Keeps what @p child's pipes hold now, and closes them.
    static void closeStreams(Child& child)
    {
        for (auto& stream: child.streams)
            if (stream.pipe.isOpen())
            {
                readStream(stream);
                stream.pipe.reset();
            }
    }

    /// Ends @p child for @p reason at once, without a signal: its leader was reaped by someone else, so its
    /// group's id may be another's by now, and only its pipes are left to give up on.
    static void abandon(Child& child, ProcessCompletion reason)
    {
        child.endedBy = reason;
        closeStreams(child);
    }

    /// Asks @p child to end: SIGTERM to its group now, SIGKILL after the grace.
    void terminate(Child& child, ProcessCompletion reason, SteadyTimePoint now) const noexcept
    {
        child.endedBy = reason;
        child.ending = Ending::Terminating;
        child.killAt = now + tuning.terminateGrace;
        child.signalGroup(SIGTERM);
    }

    /// @return When the next deadline of a process falls due by the clock, @p now for one that is due
    ///         already; std::nullopt if none will.
    [[nodiscard]] std::optional<SteadyTimePoint> nextDeadline(SteadyTimePoint now) const
    {
        auto next = std::optional<SteadyTimePoint> {};
        auto const consider = [&next](SteadyTimePoint when) {
            next = next ? std::min(*next, when) : when;
        };
        for (auto const& child: children | std::views::values)
        {
            if (child.hasEnded() || child.isReapable())
                consider(now);
            else if (child.leader == Leader::Reaped)
                consider(child.leaderExitedAt + tuning.orphanedOutputGrace);
            else if (child.ending == Ending::Terminating)
                consider(child.leader == Leader::Exited ? now : child.killAt);
            else if (child.ending == Ending::NotAsked)
            {
                consider(child.deadline);
                if (child.leader == Leader::Exited)
                    consider(child.leaderExitedAt + tuning.orphanedOutputGrace);
            }
        }
        return next;
    }

    /// @return Whether a leader's exit would go unnoticed until the next look, having no exit descriptor.
    [[nodiscard]] bool needsReapSlice() const
    {
        return std::ranges::any_of(children | std::views::values, [](Child const& child) {
            return child.leader == Leader::Running && !child.exitDescriptor.isOpen();
        });
    }

    /// Notes every leader that has exited, without reaping it.
    void lookAtLeaders(SteadyTimePoint now)
    {
        for (auto& child: children | std::views::values)
        {
            if (child.leader != Leader::Running)
                continue;
            switch (lookAtLeader(child.pid))
            {
                case LeaderLook::Running: continue;
                case LeaderLook::Exited:
                    child.leader = Leader::Exited;
                    child.leaderExitedAt = now;
                    break;
                case LeaderLook::Gone:
                    // Reaped by someone else: its exit status is lost, and its id may be reused already.
                    child.leader = Leader::Reaped;
                    child.waitStatus = std::nullopt;
                    child.leaderExitedAt = now;
                    break;
            }
            // Readable from now on; polling it any more would only spin.
            child.exitDescriptor.reset();
        }
    }

    /// Takes every process that has ended out of the table.
    [[nodiscard]] std::vector<std::pair<ProcessToken, Child>> takeEnded()
    {
        auto ended = std::vector<std::pair<ProcessToken, Child>> {};
        for (auto entry = children.begin(); entry != children.end();)
        {
            if (entry->second.hasEnded())
            {
                ended.emplace_back(entry->first, std::move(entry->second));
                entry = children.erase(entry);
            }
            else
                ++entry;
        }
        return ended;
    }

    /// Makes a waiting pump() return.
    void wake() const noexcept
    {
        auto const byte = char { 1 };
        std::ignore = ::write(wakeWrite.get(), &byte, 1);
    }

    /// Empties the wake pipe.
    void drainWake() const noexcept
    {
        auto buffer = std::array<char, 64> {};
        while (::read(wakeRead.get(), buffer.data(), buffer.size()) > 0)
        {
        }
    }

    core::platform::IClock& clock;
    ProcessRunnerTuning tuning;
    OwnedFd wakeRead;         ///< Polled by pump(); non-blocking.
    OwnedFd wakeWrite;        ///< Written by wake(); non-blocking, so a full pipe drops the byte.
    mutable std::mutex mutex; ///< Guards what follows.
    std::map<ProcessToken, Child> children; ///< Every process started and not yet completed.
    uint64_t nextToken = 1;
};

std::expected<std::unique_ptr<PipedProcessRunner>, ProcessRunnerError> PipedProcessRunner::create(
    core::platform::IClock& clock, ProcessRunnerTuning tuning)
{
    auto wakePipe = makePipe();
    if (!wakePipe || !util::setNonBlocking(wakePipe->write.get()))
        return std::unexpected(ProcessRunnerError::WakeChannelUnavailable);
    return std::make_unique<PipedProcessRunner>(Key {},
                                                std::make_unique<State>(clock, tuning, std::move(*wakePipe)));
}

PipedProcessRunner::PipedProcessRunner(Key /*key*/, std::unique_ptr<State> state) noexcept:
    _state { std::move(state) }
{
}

PipedProcessRunner::~PipedProcessRunner()
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto groups = std::vector<pid_t> {};
    for (auto& child: _state->children | std::views::values)
    {
        if (child.leader == Leader::Reaped)
            continue;
        // Unreaped, the leader holds the group's id: the signal reaches its group and no other.
        child.signalGroup(SIGKILL);
        groups.push_back(child.pid);
        reapLeader(child);
    }
    awaitGroupsGone(std::move(groups));
}

std::expected<ProcessToken, ProcessStartError> PipedProcessRunner::start(ProcessSpec spec,
                                                                         CompletionHandler done)
{
    if (spec.argv.empty())
        return std::unexpected(ProcessStartError::ProgramNotFound);
    if (spec.workingDirectory)
    {
        if (spec.workingDirectory->descriptor() < 0)
            return std::unexpected(ProcessStartError::SpawnFailed);
        if (isRemoved(*spec.workingDirectory))
            return std::unexpected(ProcessStartError::DirectoryMissing);
    }
    auto program = programOf(spec);
    if (!program)
        return std::unexpected(ProcessStartError::ProgramNotFound);

    auto output = makePipe();
    auto error = makePipe();
    if (!output || !error)
        return std::unexpected(ProcessStartError::SpawnFailed);
    auto actions = fileActionsFor(spec.workingDirectory, *output, *error);
    if (!actions)
        return std::unexpected(actions.error());
    auto attributes = attributesForChild();
    if (!attributes)
        return std::unexpected(attributes.error());

    auto arguments = std::vector<std::string> { spec.argv };
    auto argumentPointers = pointersTo(arguments);
    auto environment = environmentEntries(spec);
    auto environmentPointers = pointersTo(environment);
    auto pid = pid_t {};
    if (auto const spawned = ::posix_spawn(&pid,
                                           program->c_str(),
                                           actions->get(),
                                           attributes->get(),
                                           argumentPointers.data(),
                                           environmentPointers.data());
        spawned != 0)
        return std::unexpected(startError(spawned));
    // Our copies of the write ends close here; otherwise the pipes never report EOF.
    output->write.reset();
    error->write.reset();

    auto const capOf = [&spec](StreamCapture capture) {
        return capture == StreamCapture::Capture ? spec.outputCap : std::size_t { 0 };
    };
    auto child = Child {
        .pid = pid,
        .exitDescriptor = exitDescriptorOf(pid),
        .streams = { Stream {
                         .pipe = std::move(output->read), .cap = capOf(spec.standardOutput), .captured = {} },
                     Stream {
                         .pipe = std::move(error->read), .cap = capOf(spec.standardError), .captured = {} } },
        .deadline = _state->clock.now() + spec.timeout,
        .ending = Ending::NotAsked,
        .killAt = {},
        .endedBy = std::nullopt,
        .leader = Leader::Running,
        .leaderExitedAt = {},
        .waitStatus = std::nullopt,
        .done = std::move(done)
    };
    auto token = ProcessToken {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        token = ProcessToken { _state->nextToken++ };
        _state->children.emplace(token, std::move(child));
    }
    _state->wake();
    return token;
}

void PipedProcessRunner::cancel(ProcessToken token)
{
    {
        auto const lock = std::scoped_lock { _state->mutex };
        auto const found = _state->children.find(token);
        if (found == _state->children.end())
            return;
        auto& child = found->second;
        if (child.leader == Leader::Reaped)
            State::abandon(child, ProcessCompletion::Cancelled);
        else if (child.ending == Ending::NotAsked)
            _state->terminate(child, ProcessCompletion::Cancelled, _state->clock.now());
    }
    _state->wake();
}

void PipedProcessRunner::pump(std::chrono::milliseconds maxWait)
{
    /// Which descriptor of which process a pollfd is.
    struct Watched
    {
        ProcessToken token;
        std::optional<std::size_t> stream; ///< The stream's index; std::nullopt for the exit descriptor.
    };

    auto descriptors = std::vector<pollfd> {};
    auto watched = std::vector<Watched> {};
    auto wait = maxWait;
    {
        auto const lock = std::scoped_lock { _state->mutex };
        auto const now = _state->clock.now();
        _state->applyDeadlines(now);
        if (auto const next = _state->nextDeadline(now))
            wait = std::min(wait,
                            std::max(std::chrono::milliseconds::zero(),
                                     std::chrono::ceil<std::chrono::milliseconds>(*next - now)));
        if (_state->needsReapSlice())
            wait = std::min(wait, ReapSlice);
        descriptors.push_back(pollfd { .fd = _state->wakeRead.get(), .events = POLLIN, .revents = 0 });
        watched.push_back(Watched { .token = {}, .stream = std::nullopt });
        for (auto const& [token, child]: _state->children)
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, child.streams.size()))
                if (child.streams[index].pipe.isOpen())
                {
                    descriptors.push_back(
                        pollfd { .fd = child.streams[index].pipe.get(), .events = POLLIN, .revents = 0 });
                    watched.push_back(Watched { .token = token, .stream = index });
                }
            if (child.exitDescriptor.isOpen())
            {
                descriptors.push_back(
                    pollfd { .fd = child.exitDescriptor.get(), .events = POLLIN, .revents = 0 });
                watched.push_back(Watched { .token = token, .stream = std::nullopt });
            }
        }
    }

    // Unlocked: start() and cancel() may run meanwhile. Only pump() closes a descriptor, so none of these is
    // closed under the poll.
    if (::poll(descriptors.data(), descriptors.size(), static_cast<int>(wait.count())) < 0 && errno != EINTR)
        descriptors.clear();

    auto ended = std::vector<std::pair<ProcessToken, Child>> {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->drainWake();
        for (auto const index: std::views::iota(std::size_t { 0 }, descriptors.size()))
        {
            auto const& which = watched[index];
            if (descriptors[index].revents == 0 || !which.stream)
                continue;
            if (auto const found = _state->children.find(which.token); found != _state->children.end())
                readStream(found->second.streams[*which.stream]);
        }
        auto const now = _state->clock.now();
        _state->lookAtLeaders(now);
        _state->applyDeadlines(now);
        ended = _state->takeEnded();
    }

    // Unlocked, so that a handler may start or cancel a process.
    for (auto& [token, child]: ended)
    {
        auto done = std::move(child.done);
        done(token, std::move(child).exit());
    }
}

void PipedProcessRunner::wake() noexcept
{
    _state->wake();
}

std::size_t PipedProcessRunner::running() const noexcept
{
    auto const lock = std::scoped_lock { _state->mutex };
    return _state->children.size();
}

} // namespace vtpty
