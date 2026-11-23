// SPDX-License-Identifier: Apache-2.0
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/OverlappedPipe.hpp>
#include <vtpty/ProcessRunner.hpp>
#include <vtpty/Win32Handle.hpp>
#include <vtpty/Win32HeldDirectory.hpp>
#include <vtpty/Win32HeldSpawn.hpp>
#include <vtpty/WindowsCommandLine.hpp>
#include <vtpty/WindowsProcessRules.hpp>

#include <core/WideString.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <Windows.h>

// The Windows half of PipedProcessRunner: each process an .exe found on the spec's own PATH, started
// suspended in a kill-on-close job of its own, with NUL and the write ends of two pipes alone inherited. Both
// pipes are read by overlapped reads that complete on one I/O completion port per runner, which wake() posts
// to as well, and each leader's exit is posted to it by a thread-pool wait: pump() waits on that port alone.
//
// A process is ended by terminating its job, which ends its whole tree at once: Windows has no SIGTERM, so
// there is no graceful phase, and ProcessRunnerTuning::terminateGrace is not used. Closing the job, which
// only the runner holds, ends the tree too -- on destruction, and when the daemon dies in any way. A leader
// that exits by itself, its pipes closed, leaves what it started running, as on POSIX: its job loses
// kill-on-close before it is closed.
namespace vtpty
{

namespace
{
    using core::platform::SteadyTimePoint;

    /// How much one read takes from one pipe, as on POSIX.
    constexpr auto ReadChunkSize = std::size_t { 64 } * 1024;

    /// How many reads one pipe may complete in one round, so that a process writing without pause cannot keep
    /// the runner from its deadlines and its other processes; as POSIX's ReadChunksPerRound.
    constexpr auto ReadsPerStreamPerRound = 16;

    /// The buffer of each output pipe.
    constexpr auto PipeBufferSize = std::uint32_t { 64 } * 1024;

    /// The completion key of wake()'s packets. Tokens, which key everything else, start at 1.
    constexpr auto WakeKey = ULONG_PTR { 0 };

    /// How many completion packets one dequeue takes at most.
    constexpr auto PacketsPerDequeue = std::size_t { 64 };

    /// The exit code a terminated job's processes are given.
    constexpr auto TerminatedExitCode = UINT { 1 };

    /// The limits of a job whose leader exited by itself: closing it leaves what is left of its tree.
    constexpr auto ReleasedJobLimits = DWORD { JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION };

    /// How long the destructor waits for each terminated leader to be gone, in milliseconds; as
    /// CommandCapture's KillGracePeriod. Bounded, so that a process the system is slow to end cannot keep the
    /// daemon's shutdown from returning.
    constexpr auto KillGracePeriod = DWORD { 1000 };

    /// How a child is created: suspended, so that it is in its job before it runs a single instruction; with
    /// a console that is never shown, so that a detached daemon flashes no window per command; with a UTF-16
    /// environment block; with a handle list.
    constexpr auto CreationFlags = DWORD { CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
                                           | EXTENDED_STARTUPINFO_PRESENT };

    /// What a failed CreateProcessW() means for the start: one row per error, the rest SpawnFailed.
    constexpr auto StartErrors = std::array {
        std::pair { DWORD { ERROR_FILE_NOT_FOUND }, ProcessStartError::ProgramNotFound },
        std::pair { DWORD { ERROR_PATH_NOT_FOUND }, ProcessStartError::ProgramNotFound },
        std::pair { DWORD { ERROR_ACCESS_DENIED }, ProcessStartError::ProgramNotFound },
        std::pair { DWORD { ERROR_BAD_EXE_FORMAT }, ProcessStartError::ProgramNotFound },
        std::pair { DWORD { ERROR_EXE_MACHINE_TYPE_MISMATCH }, ProcessStartError::ProgramNotFound },
        std::pair { DWORD { ERROR_DIRECTORY }, ProcessStartError::DirectoryMissing },
    };

    /// What a held working directory refused as a child's current directory means for the start: one row per
    /// HeldDirectoryEntryError.
    constexpr auto DirectoryErrors = std::array {
        std::pair { HeldDirectoryEntryError::Unbacked, ProcessStartError::SpawnFailed },
        std::pair { HeldDirectoryEntryError::Removed, ProcessStartError::DirectoryMissing },
        std::pair { HeldDirectoryEntryError::Replaced, ProcessStartError::DirectoryMissing },
        std::pair { HeldDirectoryEntryError::TooLong, ProcessStartError::SpawnFailed },
        std::pair { HeldDirectoryEntryError::NotUtf8, ProcessStartError::SpawnFailed },
        std::pair { HeldDirectoryEntryError::Unguardable, ProcessStartError::SpawnFailed },
    };
    static_assert(DirectoryErrors.size() == std::to_underlying(HeldDirectoryEntryError::Last) + 1U
                      && std::ranges::all_of(std::views::iota(std::size_t { 0 }, DirectoryErrors.size()),
                                             [](std::size_t index) {
                                                 return std::to_underlying(DirectoryErrors.at(index).first)
                                                        == index;
                                             }),
                  "DirectoryErrors has one row per HeldDirectoryEntryError, in its order");

    /// @return What @p error, a held working directory's refusal, means for the start.
    [[nodiscard]] ProcessStartError directoryError(HeldDirectoryEntryError error) noexcept
    {
        auto const row = std::ranges::find(
            DirectoryErrors, error, &std::pair<HeldDirectoryEntryError, ProcessStartError>::first);
        return row == DirectoryErrors.end() ? ProcessStartError::SpawnFailed : row->second;
    }

    /// @return What CreateProcessW()'s failure @p error means for the start.
    [[nodiscard]] ProcessStartError startError(DWORD error) noexcept
    {
        auto const row = std::ranges::find(StartErrors, error, &std::pair<DWORD, ProcessStartError>::first);
        return row == StartErrors.end() ? ProcessStartError::SpawnFailed : row->second;
    }

    /// One read of a pipe: what the system writes into while it is pending, so held at a stable address.
    struct PendingRead
    {
        OVERLAPPED overlapped {};
        std::array<char, ReadChunkSize> buffer {};
        /// Manual-reset, set when the read completes: what cancelRead() waits on. The pipe's own handle is
        /// not signalled reliably for that once the pipe is on a completion port, and a wait on it can hang.
        UniqueHandle completed;
    };

    /// @return A PendingRead with its event; or nullptr if the event could not be made.
    [[nodiscard]] std::unique_ptr<PendingRead> makePendingRead()
    {
        auto completed = UniqueHandle { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        if (!completed.valid())
            return nullptr;
        auto read = std::make_unique<PendingRead>();
        read->completed = std::move(completed);
        return read;
    }

    /// Whether a stream has a read pending.
    enum class ReadState : uint8_t
    {
        Idle,    ///< None: one is issued in the next round.
        Pending, ///< One is: its completion packet comes through the port.
    };

    /// One output pipe of a process: the read end, its read, and what was kept.
    struct Stream
    {
        UniqueHandle pipe;                 ///< Overlapped read end, on the runner's port; closed at EOF.
        std::unique_ptr<PendingRead> read; ///< Stable while a read is pending.
        ReadState state =
            ReadState::Idle;     ///< Whether a read is pending; the pipe is never closed while one is.
        int readsThisRound = 0;  ///< How many reads it completed in the current round.
        std::size_t cap = 0;     ///< How many bytes are kept; 0 to discard.
        CapturedStream captured; ///< What was kept.

        /// Keeps what a completed read gave, up to the cap; the rest is dropped and marks it Truncated.
        void keep(std::span<char const> bytes)
        {
            auto const kept = std::min(bytes.size(), cap - std::min(cap, captured.bytes.size()));
            captured.bytes.append(bytes.data(), kept);
            if (kept < bytes.size())
                captured.extent = OutputExtent::Truncated;
        }
    };

    /// What a leader's exit wait posts to the port: allocated apart from the Child, so that its address stays
    /// what the wait was registered with however the Child moves.
    struct ExitPost
    {
        HANDLE port = nullptr; ///< The runner's completion port.
        ULONG_PTR key = 0;     ///< The process's token.
    };

    /// Posts a leader's exit to the port, from a thread-pool thread. The packet carries no OVERLAPPED, which
    /// tells it from a read's.
    VOID CALLBACK onLeaderExit(PVOID context, BOOLEAN /*timedOut*/) noexcept
    {
        auto const* const post = static_cast<ExitPost const*>(context);
        PostQueuedCompletionStatus(post->port, 0, post->key, nullptr);
    }

    /// A leader's registered exit wait, and what it posts.
    ///
    /// Unregistering waits for a callback already running (UnregisterWaitEx() with INVALID_HANDLE_VALUE):
    /// only then may the ExitPost it reads be freed and the process handle it waits on be closed. The
    /// callback takes no lock of the runner's, so the wait cannot deadlock against one held here.
    class ExitWait
    {
      public:
        ExitWait() noexcept = default;
        ExitWait(HANDLE wait, std::unique_ptr<ExitPost> post) noexcept:
            _wait { wait }, _post { std::move(post) }
        {
        }
        ExitWait(ExitWait const&) = delete;
        ExitWait& operator=(ExitWait const&) = delete;
        ExitWait(ExitWait&& other) noexcept:
            _wait { std::exchange(other._wait, nullptr) }, _post { std::move(other._post) }
        {
        }
        ExitWait& operator=(ExitWait&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                _wait = std::exchange(other._wait, nullptr);
                _post = std::move(other._post);
            }
            return *this;
        }
        ~ExitWait() { reset(); }

        /// Unregisters the wait, waiting for a callback that runs, and frees what it posts.
        void reset() noexcept
        {
            if (_wait != nullptr)
                UnregisterWaitEx(std::exchange(_wait, nullptr), INVALID_HANDLE_VALUE);
            _post.reset();
        }

      private:
        HANDLE _wait = nullptr;
        std::unique_ptr<ExitPost> _post;
    };

    /// Where a process is in being ended by the runner.
    enum class Ending : uint8_t
    {
        NotAsked, ///< Nobody has asked it to end.
        Killed,   ///< Its job was terminated (timeout, cancel, the orphaned output's grace): everything in it
                  ///< ends, the leader included.
    };

    /// Whether the leader has exited, as the runner has seen it.
    enum class Leader : uint8_t
    {
        Running, ///< Its exit has not come through the port.
        Exited,  ///< It has: its process handle is signalled.
    };

    /// A started process. Members are destroyed bottom-up: the exit wait before the process handle it waits
    /// on, the job (which ends what is left of the tree) last.
    struct Child
    {
        /// The working directory's guard, which keeps another directory from taking its name (see
        /// HeldDirectoryEntry): held until the child is reaped. None for a spec without one.
        std::optional<HeldDirectoryEntry> directory;
        UniqueHandle job;              ///< Kill-on-close (see closeJob()); held only here, never inherited.
        UniqueHandle process;          ///< The leader.
        ExitWait exitWait;             ///< Posts the leader's exit to the port.
        std::array<Stream, 2> streams; ///< Standard output, then standard error.
        SteadyTimePoint deadline;      ///< When it is timed out.
        Ending ending = Ending::NotAsked;         ///< Whether the runner has ended it.
        std::optional<ProcessCompletion> endedBy; ///< TimedOut or Cancelled, once the runner ended it.
        Leader leader = Leader::Running;          ///< Whether its leader has exited.
        std::optional<int32_t> exitCode;          ///< The leader's exit code, if it exited by itself.
        SteadyTimePoint leaderExitedAt;           ///< When the leader's exit was seen, once it was.
        CompletionHandler done;                   ///< Called with how it ended, once it has.

        /// @return Whether a pipe of it is still open.
        [[nodiscard]] bool hasOpenStream() const noexcept
        {
            return std::ranges::any_of(streams, [](Stream const& stream) { return stream.pipe.valid(); });
        }

        /// @return Whether it has ended: its leader exited and its pipes closed.
        [[nodiscard]] bool hasEnded() const noexcept { return leader == Leader::Exited && !hasOpenStream(); }

        /// @return Whether its leader exited and a pipe is still held open, by what is left of its tree (or
        ///         by a process elsewhere that inherited the write end).
        [[nodiscard]] bool isOrphaned() const noexcept { return leader == Leader::Exited && hasOpenStream(); }

        /// @return How it ended, for its handler.
        [[nodiscard]] ProcessExit exit() &&
        {
            return ProcessExit { .completion = endedBy.value_or(ProcessCompletion::Exited),
                                 .exitCode = exitCode,
                                 .signal = std::nullopt,
                                 .standardOutput = std::move(streams[0].captured),
                                 .standardError = std::move(streams[1].captured) };
        }
    };

    /// Issues a read on @p stream, Idle and open. One that is pending, or that completed at once, comes
    /// through the port either way; one that fails at once (EOF: ERROR_BROKEN_PIPE) closes the stream.
    void issueRead(Stream& stream) noexcept
    {
        stream.read->overlapped = OVERLAPPED {};
        // The completion still comes through the port: only an hEvent with its low bit set would keep it
        // away.
        stream.read->overlapped.hEvent = stream.read->completed.get();
        if (ReadFile(stream.pipe.get(),
                     stream.read->buffer.data(),
                     static_cast<DWORD>(stream.read->buffer.size()),
                     nullptr,
                     &stream.read->overlapped)
                != FALSE
            || GetLastError() == ERROR_IO_PENDING)
            stream.state = ReadState::Pending;
        else
            stream.pipe.reset();
    }

    /// Takes in the completed read of @p stream: keeps what it gave, or closes the stream at EOF or on an
    /// error. A read that gave nothing without failing (a writer's zero-byte write) leaves the stream open.
    void completeRead(Stream& stream) noexcept
    {
        stream.state = ReadState::Idle;
        ++stream.readsThisRound;
        auto count = DWORD { 0 };
        if (GetOverlappedResult(stream.pipe.get(), &stream.read->overlapped, &count, FALSE) == FALSE)
        {
            stream.pipe.reset(); // ERROR_BROKEN_PIPE is EOF; any other failure ends the stream the same way.
            return;
        }
        stream.keep(std::span { stream.read->buffer }.first(count));
    }

    /// Cancels the read pending on @p stream, if one is, and waits until the system no longer writes into it;
    /// what it read before it was cancelled is kept. Its completion packet still comes through the port, and
    /// is ignored there: the stream is no longer Pending.
    void cancelRead(Stream& stream) noexcept
    {
        if (stream.state != ReadState::Pending)
            return;
        CancelIoEx(stream.pipe.get(), &stream.read->overlapped);
        auto count = DWORD { 0 };
        if (GetOverlappedResult(stream.pipe.get(), &stream.read->overlapped, &count, TRUE) != FALSE)
            stream.keep(std::span { stream.read->buffer }.first(count));
        stream.state = ReadState::Idle;
    }

    /// Keeps what @p child's pipes gave, and closes them.
    void closeStreams(Child& child) noexcept
    {
        for (auto& stream: child.streams)
        {
            cancelRead(stream);
            stream.pipe.reset();
        }
    }

    /// Ends @p child's tree at once: its job is terminated, or, should that fail, its leader alone, so that
    /// a process the runner ends still completes.
    void terminateTree(Child const& child) noexcept
    {
        if (TerminateJobObject(child.job.get(), TerminatedExitCode) == FALSE)
            TerminateProcess(child.process.get(), TerminatedExitCode);
    }

    /// Closes @p child's job. A leader that exited by itself, nobody having asked it to end, leaves what is
    /// left of its tree running, as on POSIX (a daemon the command started, such as git's fsmonitor): its
    /// job loses kill-on-close first. Any other job ends what is left of its tree as it closes.
    void closeJob(Child& child) noexcept
    {
        if (child.ending == Ending::NotAsked)
            std::ignore = setJobLimits(child.job.get(), ReleasedJobLimits);
        child.job.reset();
    }

    /// The UTF-16 a CreateProcessW() call takes, spelled from a spec.
    struct Invocation
    {
        std::wstring program;     ///< lpApplicationName: absolute, an .exe.
        std::wstring commandLine; ///< lpCommandLine: argv[0] as given, then the arguments.
        std::wstring environment; ///< lpEnvironment: the spec's entries alone, at least two NULs.
    };

    /// @return What CreateProcessW() is to be called with for @p spec; or why it cannot be started.
    [[nodiscard]] std::expected<Invocation, ProcessStartError> invocationOf(ProcessSpec const& spec)
    {
        // The PATH searched is read from the collapsed entries, so it is the PATH the child sees.
        auto const entries = windowsEnvironmentEntries(spec.environment);
        auto const program = findWindowsHeldDirectoryProgram(
            spec.argv.front(), windowsSearchPathOf(spec.environment), isExecutableProgram);
        if (!program)
            return std::unexpected(ProcessStartError::ProgramNotFound);
        auto wideProgram = core::toUtf16(*program);
        auto commandLine =
            core::toUtf16(buildWindowsCommandLine(spec.argv.front(), std::span { spec.argv }.subspan(1)));
        auto environment = windowsEnvironmentBlock(entries);
        if (!wideProgram || !commandLine || commandLine->size() > MaxWindowsCommandLineUnits || !environment)
            return std::unexpected(ProcessStartError::SpawnFailed);
        // An empty environment is still a block, never nullptr: that would hand over the daemon's.
        if (environment->size() < 2)
            environment->resize(2, L'\0');
        return Invocation { .program = std::move(*wideProgram),
                            .commandLine = std::move(*commandLine),
                            .environment = std::move(*environment) };
    }

    /// What a child is handed as its standard handles: NUL, and the write ends of its two pipes. Not
    /// inheritable, but for the instant around CreateProcessW() (see createSuspended()).
    struct ChildEnds
    {
        UniqueHandle input;
        UniqueHandle output;
        UniqueHandle error;
    };

    /// Starts @p invocation suspended, inheriting @p ends alone, in @p directory.
    ///
    /// The ends are made inheritable only immediately before the call, and closed in this process immediately
    /// after it, success or not: the handle list keeps them from this child's siblings, but a CreateProcess()
    /// elsewhere in the daemon that passes bInheritHandles without a list would inherit any inheritable
    /// handle of ours, and so hold a pipe of this child open.
    /// @return The process and its initial thread; or why it could not be started.
    [[nodiscard]] std::expected<PROCESS_INFORMATION, ProcessStartError> createSuspended(
        Invocation& invocation, ChildEnds ends, std::optional<HeldDirectoryEntry> const& directory)
    {
        auto inherited = std::array<HANDLE, 3> { ends.input.get(), ends.output.get(), ends.error.get() };
        auto attributes = AttributeList::create(1);
        if (!attributes || !attributes->inheritOnly(inherited))
            return std::unexpected(ProcessStartError::SpawnFailed);
        if (std::ranges::any_of(inherited, [](HANDLE handle) {
                return !setInheritance(handle, HandleInheritance::Inherited).has_value();
            }))
            return std::unexpected(ProcessStartError::SpawnFailed);

        auto startup = STARTUPINFOEXW {};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = ends.input.get();
        startup.StartupInfo.hStdOutput = ends.output.get();
        startup.StartupInfo.hStdError = ends.error.get();
        startup.lpAttributeList = attributes->get();
        auto info = PROCESS_INFORMATION {};
        auto const created = CreateProcessW(invocation.program.c_str(),
                                            invocation.commandLine.data(),
                                            nullptr,
                                            nullptr,
                                            TRUE,
                                            CreationFlags,
                                            invocation.environment.data(),
                                            directory ? directory->path.c_str() : nullptr,
                                            &startup.StartupInfo,
                                            &info);
        auto const error = GetLastError();
        ends = ChildEnds {};
        if (created == FALSE)
            return std::unexpected(startError(error));
        return info;
    }

    /// @return NUL, for reading, not inheritable; or why it could not be opened.
    [[nodiscard]] std::expected<UniqueHandle, std::error_code> openNullInput()
    {
        auto nul = UniqueHandle { CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr) };
        if (!nul.valid())
            return std::unexpected(lastError());
        return nul;
    }

    /// @return The time to wait until @p next, from @p now, in whole milliseconds, never negative.
    [[nodiscard]] std::chrono::milliseconds until(SteadyTimePoint next, SteadyTimePoint now) noexcept
    {
        return std::max(std::chrono::milliseconds::zero(),
                        std::chrono::ceil<std::chrono::milliseconds>(next - now));
    }
} // namespace

struct PipedProcessRunner::State
{
    State(core::platform::IClock& clock, ProcessRunnerTuning tuning, UniqueHandle port) noexcept:
        clock { clock }, tuning { tuning }, port { std::move(port) }
    {
    }

    State(State const&) = delete;
    State(State&&) = delete;
    State& operator=(State const&) = delete;
    State& operator=(State&&) = delete;

    /// Ends every process tree and forgets it, its handler uncalled: every job is terminated first, so that
    /// the trees end together; then each exit wait is unregistered, the reads pending on its pipes are
    /// cancelled and awaited before the buffers they write into are freed, and its leader is given
    /// KillGracePeriod to be gone before its job is closed.
    ~State()
    {
        auto const lock = std::scoped_lock { mutex };
        for (auto const& child: children | std::views::values)
            terminateTree(child);
        for (auto& child: children | std::views::values)
        {
            child.exitWait.reset();
            for (auto& stream: child.streams)
                cancelRead(stream);
            WaitForSingleObject(child.process.get(), KillGracePeriod);
        }
        children.clear();
    }

    /// Ends what is due by @p now: times out a process past its deadline, and gives up on the pipes of one
    /// whose leader exited longer than orphanedOutputGrace ago, ending what is left of its tree and keeping
    /// what was read.
    void applyDeadlines(SteadyTimePoint now)
    {
        for (auto& child: children | std::views::values)
        {
            if (child.ending == Ending::NotAsked && now >= child.deadline)
                terminate(child, ProcessCompletion::TimedOut);
            // Whether the runner ended it or not: after a kill, a process outside the job may still hold a
            // pipe it inherited.
            if (child.isOrphaned() && now >= child.leaderExitedAt + tuning.orphanedOutputGrace)
            {
                // Killed, so that closing the job ends whatever of the tree a failed termination left; the
                // leader exited by itself, so its exit code and Exited stand.
                child.ending = Ending::Killed;
                terminateTree(child);
                closeStreams(child);
            }
        }
    }

    /// Ends @p child's tree at once, for @p reason. A leader that had exited by itself keeps its exit code,
    /// even if its exit has not come through the port yet.
    void terminate(Child& child, ProcessCompletion reason) const noexcept
    {
        if (child.leader == Leader::Running && WaitForSingleObject(child.process.get(), 0) == WAIT_OBJECT_0)
            noteLeaderExit(child);
        child.endedBy = reason;
        child.ending = Ending::Killed;
        terminateTree(child);
    }

    /// Issues a read on every open, Idle stream that has not had its share of this round.
    void issueReads() noexcept
    {
        for (auto& child: children | std::views::values)
            for (auto& stream: child.streams)
                if (stream.pipe.valid() && stream.state == ReadState::Idle
                    && stream.readsThisRound < ReadsPerStreamPerRound)
                    issueRead(stream);
    }

    /// Starts a round: every stream may complete its share of reads again.
    void startRound() noexcept
    {
        for (auto& child: children | std::views::values)
            for (auto& stream: child.streams)
                stream.readsThisRound = 0;
    }

    /// @return When the next deadline of a process falls due by the clock, @p now for one that has ended
    ///         already; std::nullopt if none will. A killed process has none but its orphan's grace: the
    ///         port tells when its tree has gone.
    [[nodiscard]] std::optional<SteadyTimePoint> nextDeadline(SteadyTimePoint now) const
    {
        auto next = std::optional<SteadyTimePoint> {};
        auto const consider = [&next](SteadyTimePoint when) {
            next = next ? std::min(*next, when) : when;
        };
        for (auto const& child: children | std::views::values)
        {
            if (child.hasEnded())
                consider(now);
            else
            {
                if (child.ending == Ending::NotAsked)
                    consider(child.deadline);
                if (child.isOrphaned())
                    consider(child.leaderExitedAt + tuning.orphanedOutputGrace);
            }
        }
        return next;
    }

    /// Takes in one completion packet.
    void take(OVERLAPPED_ENTRY const& packet)
    {
        if (packet.lpCompletionKey == WakeKey)
            return;
        // A packet of a process that has completed is ignored, its OVERLAPPED never dereferenced.
        auto const found = children.find(ProcessToken { packet.lpCompletionKey });
        if (found == children.end())
            return;
        auto& child = found->second;
        if (packet.lpOverlapped == nullptr)
        {
            noteLeaderExit(child);
            return;
        }
        for (auto& stream: child.streams)
            if (stream.state == ReadState::Pending && &stream.read->overlapped == packet.lpOverlapped)
                completeRead(stream);
    }

    /// Notes that @p child's leader has exited, and how: its exit code, if it exited by itself rather than
    /// by the runner terminating its job.
    void noteLeaderExit(Child& child) const noexcept
    {
        if (child.leader == Leader::Exited)
            return;
        child.leader = Leader::Exited;
        child.leaderExitedAt = clock.now();
        auto code = DWORD { 0 };
        // The exit is told by the handle being signalled, never by STILL_ACTIVE, which is a legal exit code.
        if (child.ending == Ending::NotAsked && GetExitCodeProcess(child.process.get(), &code) != FALSE)
            child.exitCode = static_cast<int32_t>(code);
    }

    /// Dequeues the packets the port holds, waiting @p wait at most for the first, and takes them in.
    /// @return How many there were.
    [[nodiscard]] std::size_t dequeue(std::span<OVERLAPPED_ENTRY> packets, DWORD wait) const noexcept
    {
        auto count = ULONG { 0 };
        if (GetQueuedCompletionStatusEx(
                port.get(), packets.data(), static_cast<ULONG>(packets.size()), &count, wait, FALSE)
            == FALSE)
            return 0;
        return count;
    }

    /// Takes every process that has ended out of the table, its exit wait unregistered and its job closed
    /// (see closeJob()).
    [[nodiscard]] std::vector<std::pair<ProcessToken, Child>> takeEnded()
    {
        auto ended = std::vector<std::pair<ProcessToken, Child>> {};
        for (auto& [token, child]: children)
        {
            if (!child.hasEnded())
                continue;
            child.exitWait.reset();
            closeJob(child);
            ended.emplace_back(token, std::move(child));
        }
        for (auto const& entry: ended)
            children.erase(entry.first);
        return ended;
    }

    /// Makes a waiting pump() return.
    void wake() const noexcept { PostQueuedCompletionStatus(port.get(), 0, WakeKey, nullptr); }

    core::platform::IClock& clock;
    ProcessRunnerTuning tuning;
    UniqueHandle port;        ///< Every read, every leader's exit and every wake() completes here.
    mutable std::mutex mutex; ///< Guards what follows.
    std::map<ProcessToken, Child> children; ///< Every process started and not yet completed.
    uint64_t nextToken = 1;
};

std::expected<std::unique_ptr<PipedProcessRunner>, ProcessRunnerError> PipedProcessRunner::create(
    core::platform::IClock& clock, ProcessRunnerTuning tuning)
{
    auto port = UniqueHandle { CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1) };
    if (!port.valid())
        return std::unexpected(ProcessRunnerError::WakeChannelUnavailable);
    return std::make_unique<PipedProcessRunner>(Key {},
                                                std::make_unique<State>(clock, tuning, std::move(port)));
}

PipedProcessRunner::PipedProcessRunner(Key /*key*/, std::unique_ptr<State> state) noexcept:
    _state { std::move(state) }
{
}

PipedProcessRunner::~PipedProcessRunner() = default;

std::expected<ProcessToken, ProcessStartError> PipedProcessRunner::start(ProcessSpec spec,
                                                                         CompletionHandler done)
{
    if (spec.argv.empty())
        return std::unexpected(ProcessStartError::ProgramNotFound);
    auto directory = std::optional<HeldDirectoryEntry> {};
    if (spec.workingDirectory)
    {
        auto entry = prepareHeldDirectoryEntry(*spec.workingDirectory);
        if (!entry)
            return std::unexpected(directoryError(entry.error()));
        directory = std::move(*entry);
    }
    auto invocation = invocationOf(spec);
    if (!invocation)
        return std::unexpected(invocation.error());

    auto output = createOverlappedPipe(PipeDirection::Inbound, L"runner-stdout", PipeBufferSize);
    auto error = createOverlappedPipe(PipeDirection::Inbound, L"runner-stderr", PipeBufferSize);
    auto input = openNullInput();
    auto job = createKillOnCloseJob();
    auto reads = std::array { makePendingRead(), makePendingRead() };
    if (!output || !error || !input || !job || !reads[0] || !reads[1])
        return std::unexpected(ProcessStartError::SpawnFailed);

    auto const created = createSuspended(*invocation,
                                         ChildEnds { .input = std::move(*input),
                                                     .output = std::move(output->theirs),
                                                     .error = std::move(error->theirs) },
                                         directory);
    if (!created)
        return std::unexpected(created.error());
    auto process = UniqueHandle { created->hProcess };
    auto const thread = UniqueHandle { created->hThread };

    // The child is suspended: what fails from here on ends it before it ran.
    auto const kill = [&process](ProcessStartError why) {
        TerminateProcess(process.get(), 1);
        return std::unexpected(why);
    };
    // Looked at again with the child suspended, redundantly while the guard holds the name: cheap.
    if (directory)
    {
        if (auto const confirmed = confirmHeldDirectoryEntry(*directory); !confirmed)
            return kill(directoryError(confirmed.error()));
    }
    if (AssignProcessToJobObject(job->get(), process.get()) == FALSE
        || ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        return kill(ProcessStartError::SpawnFailed);
    // From here on the child is in its job, which closing on an early return ends.

    auto token = ProcessToken {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        token = ProcessToken { _state->nextToken++ };
    }
    auto const key = static_cast<ULONG_PTR>(std::to_underlying(token));
    for (auto const* const pipe: { &output->ours, &error->ours })
        if (CreateIoCompletionPort(pipe->get(), _state->port.get(), key, 0) == nullptr)
            return std::unexpected(ProcessStartError::SpawnFailed);

    auto const capOf = [&spec](StreamCapture capture) {
        return capture == StreamCapture::Capture ? spec.outputCap : std::size_t { 0 };
    };
    auto const streamOf =
        [&](OverlappedPipe& pipe, std::unique_ptr<PendingRead>& read, StreamCapture capture) {
            return Stream { .pipe = std::move(pipe.ours),
                            .read = std::move(read),
                            .state = ReadState::Idle,
                            .readsThisRound = 0,
                            .cap = capOf(capture),
                            .captured = {} };
        };
    auto child = Child { .directory = std::move(directory),
                         .job = std::move(*job),
                         .process = std::move(process),
                         .exitWait = {},
                         .streams = { streamOf(*output, reads[0], spec.standardOutput),
                                      streamOf(*error, reads[1], spec.standardError) },
                         .deadline = _state->clock.now() + spec.timeout,
                         .ending = Ending::NotAsked,
                         .endedBy = std::nullopt,
                         .leader = Leader::Running,
                         .exitCode = std::nullopt,
                         .leaderExitedAt = {},
                         .done = std::move(done) };
    {
        // Registered under the lock and inserted before it is released: a leader that exits at once posts a
        // packet that pump() takes in only once the child is in the table.
        auto const lock = std::scoped_lock { _state->mutex };
        auto post = std::make_unique<ExitPost>(ExitPost { .port = _state->port.get(), .key = key });
        auto wait = HANDLE { nullptr };
        if (RegisterWaitForSingleObject(
                &wait, child.process.get(), onLeaderExit, post.get(), INFINITE, WT_EXECUTEONLYONCE)
            == FALSE)
            return std::unexpected(ProcessStartError::SpawnFailed);
        child.exitWait = ExitWait { wait, std::move(post) };
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
        if (child.hasEnded() || child.ending != Ending::NotAsked)
            return;
        _state->terminate(child, ProcessCompletion::Cancelled);
    }
    _state->wake();
}

void PipedProcessRunner::pump(std::chrono::milliseconds maxWait)
{
    auto packets = std::array<OVERLAPPED_ENTRY, PacketsPerDequeue> {};
    auto wait = maxWait;
    {
        auto const lock = std::scoped_lock { _state->mutex };
        auto const now = _state->clock.now();
        _state->applyDeadlines(now);
        _state->startRound();
        _state->issueReads();
        if (auto const next = _state->nextDeadline(now))
            wait = std::min(wait, until(*next, now));
    }

    // Unlocked: start() and cancel() may run meanwhile. Only pump() closes a pipe or frees a read, so none
    // that a packet names goes away under the wait.
    auto const waitMilliseconds =
        static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(wait.count(), 0, INFINITE - 1));
    auto const first = _state->dequeue(packets, waitMilliseconds);

    auto ended = std::vector<std::pair<ProcessToken, Child>> {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        auto const takeIn = [&](std::size_t count) {
            for (auto const& packet: std::span { packets }.first(count))
                _state->take(packet);
        };
        // What came, then what the reads issued since give at once, up to each stream's share of the round.
        auto count = first;
        for ([[maybe_unused]] auto const drain: std::views::iota(0, ReadsPerStreamPerRound))
        {
            takeIn(std::exchange(count, 0));
            _state->issueReads();
            count = _state->dequeue(packets, 0);
            if (count == 0)
                break;
        }
        takeIn(count); // Every packet dequeued is taken in: a read's would otherwise be lost.
        _state->applyDeadlines(_state->clock.now());
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
