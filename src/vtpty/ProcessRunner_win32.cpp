// SPDX-License-Identifier: Apache-2.0
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/OverlappedPipe.hpp>
#include <vtpty/ProcessRunner.hpp>
#include <vtpty/Win32Handle.hpp>
#include <vtpty/WindowsCommandLine.hpp>
#include <vtpty/WindowsProcessRules.hpp>

#include <core/Utf8Path.hpp>
#include <core/WideString.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <algorithm>
#include <array>
#include <bit>
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
#include <utility>
#include <vector>

#include <Windows.h>

// The Windows half of PipedProcessRunner: each process an .exe found on the spec's own PATH, started
// suspended in a kill-on-close job of its own, with NUL and the write ends of two pipes alone inherited. Both
// pipes are read by overlapped reads that complete on one I/O completion port per runner, which wake() posts
// to as well, and each leader's exit is posted to it by a thread-pool wait: pump() waits on that port alone.
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

    /// The longest command line CreateProcessW() takes, in UTF-16 units, its terminating NUL not counted.
    constexpr auto MaxCommandLine = std::size_t { 32766 };

    /// The completion key of wake()'s packets. Tokens, which key everything else, start at 1.
    constexpr auto WakeKey = ULONG_PTR { 0 };

    /// How many completion packets one dequeue takes at most.
    constexpr auto PacketsPerDequeue = std::size_t { 64 };

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
    };

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
        UniqueHandle job;                         ///< Kill-on-close; held only here, never inherited.
        UniqueHandle process;                     ///< The leader.
        ExitWait exitWait;                        ///< Posts the leader's exit to the port.
        std::array<Stream, 2> streams;            ///< Standard output, then standard error.
        SteadyTimePoint deadline;                 ///< When it is timed out.
        std::optional<ProcessCompletion> endedBy; ///< TimedOut or Cancelled, once the runner ended it.
        Leader leader = Leader::Running;          ///< Whether its leader has exited.
        std::optional<int32_t> exitCode;          ///< The leader's exit code, once it exited by itself.
        SteadyTimePoint leaderExitedAt;           ///< When the leader's exit was seen, once it was.
        CompletionHandler done;

        /// @return Whether a pipe of it is still open.
        [[nodiscard]] bool hasOpenStream() const noexcept
        {
            return std::ranges::any_of(streams, [](Stream const& stream) { return stream.pipe.valid(); });
        }

        /// @return Whether it has ended: its leader exited and its pipes closed.
        [[nodiscard]] bool hasEnded() const noexcept { return leader == Leader::Exited && !hasOpenStream(); }

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

    /// Cancels the read pending on @p stream, if one is, and waits until the system no longer writes into it.
    void cancelRead(Stream& stream) noexcept
    {
        if (stream.state != ReadState::Pending)
            return;
        CancelIoEx(stream.pipe.get(), &stream.read->overlapped);
        auto count = DWORD { 0 };
        GetOverlappedResult(stream.pipe.get(), &stream.read->overlapped, &count, TRUE);
        stream.state = ReadState::Idle;
    }

    /// @return Which file @p handle is, by volume and file id; std::nullopt where neither can be read.
    [[nodiscard]] std::optional<FILE_ID_INFO> fileIdOf(HANDLE handle) noexcept
    {
        auto id = FILE_ID_INFO {};
        if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) != FALSE)
            return id;
        // A file system without 128-bit ids (FAT) still has the 64-bit index.
        auto info = BY_HANDLE_FILE_INFORMATION {};
        if (GetFileInformationByHandle(handle, &info) == FALSE)
            return std::nullopt;
        id = FILE_ID_INFO {};
        id.VolumeSerialNumber = info.dwVolumeSerialNumber;
        auto const index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
        id.FileId = std::bit_cast<FILE_ID_128>(std::array { index, std::uint64_t { 0 } });
        return id;
    }

    /// Whether the directory at a held handle's path is the held one.
    enum class DirectoryAtPath : uint8_t
    {
        Other = 0, ///< Another directory, or nothing, stands at its path.
        Held,      ///< The held directory does.
    };

    /// Checks that @p directory's resolvedPath(), which is all CreateProcessW() takes, still names the
    /// directory it holds open: the path is opened and its file id compared with the handle's. Run before
    /// CreateProcessW() and again after it, while the child is still suspended, so that a directory swapped
    /// in under the name at any time before the child could run is refused.
    ///
    /// Residual: a swap of the name to another directory and back again, both within the CreateProcessW()
    /// call, goes unseen -- the child would run in the other directory. Only a handle-based process creation
    /// could close that, and Win32 has none.
    [[nodiscard]] DirectoryAtPath directoryAtPath(DirectoryHandle const& directory) noexcept
    {
        auto const path = core::toUtf16(directory.resolvedPath());
        if (!path)
            return DirectoryAtPath::Other;
        auto const opened = UniqueHandle { CreateFileW(path->c_str(),
                                                       FILE_READ_ATTRIBUTES,
                                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                       nullptr,
                                                       OPEN_EXISTING,
                                                       FILE_FLAG_BACKUP_SEMANTICS,
                                                       nullptr) };
        if (!opened.valid())
            return DirectoryAtPath::Other;
        auto const held = fileIdOf(directory.nativeHandle());
        auto const atPath = fileIdOf(opened.get());
        auto const same = held && atPath && held->VolumeSerialNumber == atPath->VolumeSerialNumber
                          && std::ranges::equal(held->FileId.Identifier, atPath->FileId.Identifier);
        return same ? DirectoryAtPath::Held : DirectoryAtPath::Other;
    }

    /// @return Whether @p candidate, absolute and UTF-8, names a file to run.
    [[nodiscard]] bool isExecutable(std::string const& candidate)
    {
        auto const path = core::pathFromUtf8(candidate);
        return path && core::platform::NativeFileSystem::instance().isExecutableFile(*path);
    }

    /// @return A kill-on-close job; or why it could not be made.
    [[nodiscard]] std::expected<UniqueHandle, std::error_code> createKillOnCloseJob()
    {
        auto job = UniqueHandle { CreateJobObjectW(nullptr, nullptr) };
        if (!job.valid())
            return std::unexpected(lastError());
        auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))
            == FALSE)
            return std::unexpected(lastError());
        return job;
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
            spec.argv.front(), windowsSearchPathOf(spec.environment), isExecutable);
        if (!program)
            return std::unexpected(ProcessStartError::ProgramNotFound);
        auto wideProgram = core::toUtf16(*program);
        auto commandLine =
            core::toUtf16(buildWindowsCommandLine(spec.argv.front(), std::span { spec.argv }.subspan(1)));
        auto environment = windowsEnvironmentBlock(entries);
        if (!wideProgram || !commandLine || commandLine->size() > MaxCommandLine || !environment)
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
        Invocation& invocation, ChildEnds ends, std::optional<std::wstring> const& directory)
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
                                            directory ? directory->c_str() : nullptr,
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

    /// Ends every process and forgets it, its handler uncalled: the reads pending on its pipes are cancelled
    /// and awaited before the buffers they write into are freed.
    ~State()
    {
        auto const lock = std::scoped_lock { mutex };
        for (auto& child: children | std::views::values)
        {
            TerminateJobObject(child.job.get(), 1);
            child.exitWait.reset();
            for (auto& stream: child.streams)
                cancelRead(stream);
        }
        children.clear();
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

    /// @return When the next deadline falls due by the clock, @p now for a process that has ended already;
    ///         std::nullopt if none will.
    [[nodiscard]] std::optional<SteadyTimePoint> nextDeadline(SteadyTimePoint now) const
    {
        auto const anyEnded = std::ranges::any_of(children | std::views::values,
                                                  [](Child const& child) { return child.hasEnded(); });
        return anyEnded ? std::optional { now } : std::nullopt;
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

    /// Notes that @p child's leader has exited, and how.
    void noteLeaderExit(Child& child) const
    {
        if (child.leader == Leader::Exited)
            return;
        child.leader = Leader::Exited;
        child.leaderExitedAt = clock.now();
        auto code = DWORD { 0 };
        // The exit is told by the handle being signalled, never by STILL_ACTIVE, which is a legal exit code.
        if (GetExitCodeProcess(child.process.get(), &code) != FALSE)
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

    /// Takes every process that has ended out of the table, its exit wait unregistered and its job closed,
    /// which ends anything of its tree still running.
    [[nodiscard]] std::vector<std::pair<ProcessToken, Child>> takeEnded()
    {
        auto ended = std::vector<std::pair<ProcessToken, Child>> {};
        for (auto entry = children.begin(); entry != children.end();)
        {
            if (entry->second.hasEnded())
            {
                entry->second.exitWait.reset();
                entry->second.job.reset();
                ended.emplace_back(entry->first, std::move(entry->second));
                entry = children.erase(entry);
            }
            else
                ++entry;
        }
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
    auto directory = std::optional<std::wstring> {};
    if (spec.workingDirectory)
    {
        if (spec.workingDirectory->nativeHandle() == nullptr)
            return std::unexpected(ProcessStartError::SpawnFailed);
        if (spec.workingDirectory->isRemoved()
            || directoryAtPath(*spec.workingDirectory) != DirectoryAtPath::Held)
            return std::unexpected(ProcessStartError::DirectoryMissing);
        auto wide = core::toUtf16(spec.workingDirectory->resolvedPath());
        if (!wide)
            return std::unexpected(ProcessStartError::SpawnFailed);
        directory = std::move(*wide);
    }
    auto invocation = invocationOf(spec);
    if (!invocation)
        return std::unexpected(invocation.error());

    auto output = createOverlappedPipe(PipeDirection::Inbound, L"runner-stdout", PipeBufferSize);
    auto error = createOverlappedPipe(PipeDirection::Inbound, L"runner-stderr", PipeBufferSize);
    auto input = openNullInput();
    auto job = createKillOnCloseJob();
    if (!output || !error || !input || !job)
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
    if (spec.workingDirectory && directoryAtPath(*spec.workingDirectory) != DirectoryAtPath::Held)
        return kill(ProcessStartError::DirectoryMissing);
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
    auto const streamOf = [&](OverlappedPipe& pipe, StreamCapture capture) {
        return Stream { .pipe = std::move(pipe.ours),
                        .read = std::make_unique<PendingRead>(),
                        .state = ReadState::Idle,
                        .readsThisRound = 0,
                        .cap = capOf(capture),
                        .captured = {} };
    };
    auto child =
        Child { .job = std::move(*job),
                .process = std::move(process),
                .exitWait = {},
                .streams = { streamOf(*output, spec.standardOutput), streamOf(*error, spec.standardError) },
                .deadline = _state->clock.now() + spec.timeout,
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

void PipedProcessRunner::cancel(ProcessToken /*token*/)
{
    // Ending a process is not wired on Windows yet: a cancel is ignored, and the process runs to its end.
}

void PipedProcessRunner::pump(std::chrono::milliseconds maxWait)
{
    auto packets = std::array<OVERLAPPED_ENTRY, PacketsPerDequeue> {};
    auto wait = maxWait;
    {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->startRound();
        _state->issueReads();
        auto const now = _state->clock.now();
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
