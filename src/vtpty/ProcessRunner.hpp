// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/DirectoryHandle.hpp>

#include <core/platform/Clock.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Running external programs to completion and capturing both their streams, with deadlines read from an
// injected clock: the seam processes are reached through, so that what runs them is tested with a fake.
namespace vtpty
{

/// Names one started process to its runner.
enum class ProcessToken : uint64_t
{
};

/// How a process came to an end.
///
/// On Windows `signal` is never set: a process the runner ends has neither `exitCode` nor `signal`, unless
/// its leader had exited by itself first, in which case its `exitCode` is kept. `Signalled` and `StatusLost`
/// never occur there, since no one else can reap a process whose handle the runner holds.
enum class ProcessCompletion : uint8_t
{
    Exited, ///< It exited by itself (exitCode is set; on Windows an NTSTATUS such as 0xC0000005 is negative).
    Signalled,  ///< A signal ended it, not one the runner sent (signal is set). POSIX only.
    TimedOut,   ///< It outlived its timeout, and the runner ended it (signal is set; on Windows neither is,
                ///< unless it exited by itself first).
    Cancelled,  ///< The runner ended it on cancel() (signal is set, unless it exited by itself first; on
                ///< Windows neither is, unless it exited by itself first).
    StatusLost, ///< Its leader was reaped by someone else, so how it ended is unknown (neither is set). POSIX
                ///< only.
};

/// What becomes of what a process writes to one of its output streams.
enum class StreamCapture : uint8_t
{
    Discard, ///< Read and dropped: the stream stays empty, `Truncated` if anything was written.
    Capture, ///< Kept, up to the spec's outputCap.
};

/// Whether a captured stream holds all the process wrote to it.
enum class OutputExtent : uint8_t
{
    Whole,     ///< Everything the process wrote.
    Truncated, ///< The first outputCap bytes; the rest was read and dropped.
};

/// What a process wrote to one of its output streams.
struct CapturedStream
{
    std::string bytes;                         ///< What was kept, at most the spec's outputCap.
    OutputExtent extent = OutputExtent::Whole; ///< Whether that is all it wrote.
};

/// How a process ended, and what it wrote.
struct ProcessExit
{
    ProcessCompletion completion = ProcessCompletion::Exited; ///< How it came to an end.
    std::optional<int32_t> exitCode;                          ///< Its exit status, if it exited.
    std::optional<int32_t> signal;                            ///< The signal that ended it, if one did.
    CapturedStream standardOutput;                            ///< What it wrote to standard output.
    CapturedStream standardError;                             ///< What it wrote to standard error.
};

/// What to run, and where and how.
struct ProcessSpec
{
    /// The program, looked up on @ref environment's PATH (never this process's), followed by its arguments.
    std::vector<std::string> argv;
    /// The directory it runs in, entered by its handle; std::nullopt for the runner's own.
    std::optional<DirectoryHandle> workingDirectory;
    /// The child's whole environment: nothing of this process's is added.
    std::vector<std::pair<std::string, std::string>> environment;
    /// How long it may run before it is terminated.
    std::chrono::milliseconds timeout {};
    /// How many bytes of each captured stream are kept; the rest is drained and dropped.
    std::size_t outputCap = 0;
    /// What becomes of standard output.
    StreamCapture standardOutput = StreamCapture::Capture;
    /// What becomes of standard error.
    StreamCapture standardError = StreamCapture::Capture;
    /// Lines of standard output that begin with one of these are dropped as they are read, before
    /// @ref outputCap applies (StreamKeeper); none is empty. Empty: every line counts against the cap.
    std::vector<std::string> droppedOutputLines = {};
};

/// Why a process could not be started.
enum class ProcessStartError : uint8_t
{
    DirectoryMissing, ///< The working directory is gone.
    ProgramNotFound,  ///< The program is not on the spec's PATH, or not executable.
    SpawnFailed,      ///< The system refused to start it (resources, an unbacked directory handle).
    Unsupported,      ///< This platform (or C library) cannot run a process as specified.
};

/// Why a process runner could not be made.
enum class ProcessRunnerError : uint8_t
{
    WakeChannelUnavailable, ///< The pipe, or on Windows the completion port, that wakes a waiting pump()
                            ///< could not be made.
    ThreadUnavailable,      ///< The thread that pumps could not be started.
};

/// Called once a process has ended, on the runner's thread (never inline in start()).
using CompletionHandler = std::function<void(ProcessToken token, ProcessExit exit)>;

/// Starts processes and tells when each ends.
class IProcessRunner
{
  public:
    IProcessRunner() = default;
    IProcessRunner(IProcessRunner const&) = delete;
    IProcessRunner(IProcessRunner&&) = delete;
    IProcessRunner& operator=(IProcessRunner const&) = delete;
    IProcessRunner& operator=(IProcessRunner&&) = delete;
    virtual ~IProcessRunner() = default;

    /// Starts @p spec; @p done is called exactly once when it ends, unless the runner is destroyed first.
    /// @return The process's token; or why it could not be started, in which case @p done is never called.
    [[nodiscard]] virtual std::expected<ProcessToken, ProcessStartError> start(ProcessSpec spec,
                                                                               CompletionHandler done) = 0;

    /// Terminates the process: SIGTERM to its group now, SIGKILL to the group once its leader has ended or
    /// the grace has passed. It completes as `Cancelled` (at once, if its leader was reaped by someone else).
    /// On Windows its job, and so its whole tree, is terminated at once: there is no graceful phase.
    /// A token that has completed (or never was) is ignored.
    virtual void cancel(ProcessToken token) = 0;
};

/// How a PipedProcessRunner treats a process that does not end when it should.
struct ProcessRunnerTuning
{
    /// How long a process group's leader has between SIGTERM and SIGKILL. The group gets SIGKILL sooner, as
    /// soon as the leader has ended. Not used on Windows, which has no SIGTERM: a process is ended at once
    /// there -- so a `git` timed out mid-operation may leave `.git/index.lock` behind, which on POSIX its
    /// SIGTERM handler removes.
    std::chrono::milliseconds terminateGrace { 2000 };
    /// How long the output of a leader that has exited is still awaited while another process of its group
    /// holds a pipe open; then the group is killed and the process completes with what was read.
    std::chrono::milliseconds orphanedOutputGrace { 1000 };
};

/// Runs processes on pipes, each the leader of a session of its own (a process group where the C library
/// cannot start a session: `/dev/tty` then stops rather than fails), with no thread of its own: pump() does
/// one round. Deterministic under a ManualClock: every deadline is read from the injected clock.
///
/// A process timed out or cancelled gets SIGTERM to its group, and SIGKILL to its group as soon as its leader
/// has ended, or once the grace has passed if it has not: the leader is reaped only after that, so that its
/// id, which is the group's, cannot be reused by an unrelated process the SIGKILL would reach. A leader that
/// ends on SIGTERM completes within a round; one that ignores it, after the grace.
///
/// On Windows each process runs in a kill-on-close Job Object of its own, which only the runner holds. A
/// process timed out or cancelled has its job terminated at once, which ends its whole tree, grandchildren
/// included; closing the job -- on destruction, or when the process holding the runner dies in any way --
/// ends it too, as does the orphaned output's grace running out. A leader that exits by itself, its pipes
/// closed, leaves what it started running, as on POSIX (a daemon such as git's fsmonitor or gpg-agent): its
/// job loses kill-on-close before the runner closes it. A process in a job that raises an exception nothing
/// handles ends at once, rather than waiting on Windows Error Reporting. Only a program whose name ends in
/// `.exe` is started, found on the spec's PATH's absolute entries alone; a `.cmd` or `.bat` is refused, so no
/// argument ever passes through `cmd.exe`.
///
/// start(), cancel() and running() may be called from any thread, pump() from one at a time.
class PipedProcessRunner final: public IProcessRunner
{
    struct State;

    /// Lets only create() construct a runner, through std::make_unique.
    struct Key
    {
        explicit Key() = default;
    };

  public:
    /// @param clock What every deadline is measured by; must outlive the runner.
    /// @param tuning How a process that does not end is ended.
    /// @return The runner; or why it could not be made.
    [[nodiscard]] static std::expected<std::unique_ptr<PipedProcessRunner>, ProcessRunnerError> create(
        core::platform::IClock& clock, ProcessRunnerTuning tuning);

    /// For create() only.
    PipedProcessRunner(Key key, std::unique_ptr<State> state) noexcept;

    /// SIGKILLs every process group, reaps every process, and calls no handler. On Windows: terminates
    /// every job and closes it, waiting at most a second for each leader to be gone.
    ~PipedProcessRunner() override;

    [[nodiscard]] std::expected<ProcessToken, ProcessStartError> start(ProcessSpec spec,
                                                                       CompletionHandler done) override;
    void cancel(ProcessToken token) override;

    /// Reads every pipe, applies deadlines by the clock, reaps, and calls the handlers of what ended.
    /// @param maxWait How long it waits at most for something to happen; it returns sooner when a
    ///        deadline falls due by the clock, a process ends, or wake() is called.
    void pump(std::chrono::milliseconds maxWait);

    /// Makes a pump() waiting on another thread return now.
    void wake() noexcept;

    /// @return How many processes have started and not yet completed.
    [[nodiscard]] std::size_t running() const noexcept;

  private:
    std::unique_ptr<State> _state;
};

/// Production: a PipedProcessRunner pumped by a thread of its own, woken on start and cancel.
class ThreadedProcessRunner final: public IProcessRunner
{
    /// Lets only create() construct a runner, through std::make_unique.
    struct Key
    {
        explicit Key() = default;
    };

  public:
    /// @param clock What every deadline is measured by; must outlive the runner.
    /// @param tuning How a process that does not end is ended.
    /// @return The runner, its thread started; or why it could not be made.
    [[nodiscard]] static std::expected<std::unique_ptr<ThreadedProcessRunner>, ProcessRunnerError> create(
        core::platform::IClock& clock, ProcessRunnerTuning tuning);

    /// For create() only; starts the thread, throwing std::system_error if it cannot.
    ThreadedProcessRunner(Key key, std::unique_ptr<PipedProcessRunner> runner);

    /// Stops and joins the thread, then kills and reaps as ~PipedProcessRunner() does.
    ~ThreadedProcessRunner() override;

    [[nodiscard]] std::expected<ProcessToken, ProcessStartError> start(ProcessSpec spec,
                                                                       CompletionHandler done) override;
    void cancel(ProcessToken token) override;

  private:
    /// Whether the pumping thread is to keep going.
    enum class Pumping : uint8_t
    {
        Stopping,
        Running,
    };

    std::unique_ptr<PipedProcessRunner> _runner;
    std::atomic<Pumping> _pumping { Pumping::Running };
    std::thread _thread;
};

} // namespace vtpty
