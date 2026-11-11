// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <vthost/keeper/Frame.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>

/// The keeper's ports: everything it reaches of the operating system, as interfaces. The keeper's logic is
/// written against these alone, so it runs with fakes in tests; the POSIX adapters implement them.
namespace vthost::keeper
{

/// The exclusive lock on a label (`<socket path>.keeper.lock`), held for as long as the object lives.
class LabelLock
{
  public:
    virtual ~LabelLock() = default;
};

/// The keeper's end of the private connection to one daemon process.
class Channel
{
  public:
    virtual ~Channel() = default;

    /// Queues @p frame for the daemon. Never blocks; a daemon that does not read is dropped, and that shows
    /// as EventKind::ChannelClosed.
    /// @param frame The frame. Its descriptors stay the caller's: the channel duplicates what it must keep
    ///              before it returns.
    virtual void send(Frame frame) = 0;

    /// @return What an event source waits on for this channel.
    [[nodiscard]] virtual int pollHandle() const = 0;
};

/// A daemon process the launcher started, and the keeper's end of its channel.
struct LaunchedDaemon
{
    int pid = 0;
    std::unique_ptr<Channel> channel;
};

/// Starts daemon processes.
class DaemonLauncher
{
  public:
    virtual ~DaemonLauncher() = default;

    /// Starts a daemon from @p image with the frozen argv and a fresh channel.
    /// @param image The daemon executable.
    /// @return The started daemon, or why none started.
    [[nodiscard]] virtual std::expected<LaunchedDaemon, SpawnError> launch(
        std::filesystem::path const& image) = 0;
};

/// A child the spawner forked.
struct SpawnedChild
{
    int pid = 0;
    UniqueFd processHandle; ///< Its pidfd where the platform has one; none otherwise.
};

/// Whose process a watched one is.
enum class WatchedRole : uint8_t
{
    Session = 0, ///< A shell the keeper forked for the daemon.
    Daemon,      ///< The daemon, whose channel holds what it wrote before it ended.
};

/// A process for EventSource::watchPid().
struct WatchedProcess
{
    int pid = 0;
    WatchedRole role = WatchedRole::Session;
    /// Its pidfd, where its spawn opened one (SpawnedChild::processHandle): borrowed, so that no second one
    /// is opened beside it, and kept open by its owner until reap(). -1 to have the event source open one.
    int processHandle = -1;
};

/// Forks the shells the daemon asks for.
class ShellSpawner
{
  public:
    virtual ~ShellSpawner() = default;

    /// Forks and execs the child @p request describes.
    /// @param request     The daemon's Spawn.
    /// @param descriptors The child's descriptors: descriptors[n] becomes child descriptor
    ///                    request.childFds[n]. They stay the caller's; the spawner closes none of them in
    ///                    this process.
    /// @param executable  The executable, for ExecutableSource::Descriptor; nullopt to run
    ///                    request.executablePath. It stays the caller's.
    /// @return The child, or why it could not be started.
    [[nodiscard]] virtual std::expected<SpawnedChild, SpawnError> spawn(Spawn const& request,
                                                                        std::span<int const> descriptors,
                                                                        std::optional<int> executable) = 0;
};

/// What woke the keeper.
enum class EventKind : uint8_t
{
    Frame = 0,     ///< A frame arrived on the daemon's channel.
    ChannelClosed, ///< The daemon's channel ended, or broke.
    Ended, ///< A watched process ended; it is a zombie until the keeper reaps it (@see EventSource::reap).
    StopSignal, ///< The keeper was asked to stop (SIGTERM, SIGINT, SIGHUP, the console-control event).
    Timer,      ///< The deadline passed.
};

/// Which daemon process a channel belongs to; minted by the keeper per launch, never reused. Zero is none.
enum class DaemonGeneration : uint64_t
{
};

/// One thing that happened.
struct Event
{
    EventKind kind = EventKind::Timer;
    std::optional<Frame> frame;     ///< For Frame: the frame, its descriptors now the keeper's.
    int pid = 0;                    ///< For Ended: the process.
    WireExitStatus status {};       ///< For Ended: how it ended.
    DaemonGeneration generation {}; ///< For Frame and ChannelClosed: whose channel it came from.
};

/// The keeper's one wait: on the daemon's channel, its children and its stop signals.
///
/// Ordering contract: a daemon's remaining frames and its ChannelClosed are reported before its Ended, so
/// that the keeper reads a Shutdown the daemon wrote before it judges the exit. The keeper tolerates the
/// reverse order for ChannelDrainBound, and drops frames and closes of a generation that is not current.
class EventSource
{
  public:
    virtual ~EventSource() = default;

    /// Starts reporting the frames and the end of @p channel, each tagged with @p generation.
    virtual void watchChannel(Channel& channel, DaemonGeneration generation) = 0;

    /// Stops reporting @p channel; called before the keeper destroys it.
    virtual void unwatchChannel(Channel& channel) = 0;

    /// Reports a process's end, by pid, pidfd or kqueue -- never `waitpid(-1)`.
    ///
    /// The exit status is read without reaping (`waitid(WNOWAIT)`, a pidfd): the zombie is what keeps the pid
    /// from being reused while the keeper may still signal it -- a signal handled from a frame of the same
    /// wait() as the Ended, but before it, finds the zombie, never another process. The keeper reaps it with
    /// reap() once it has handled the Ended and will send it nothing more.
    ///
    /// The daemon's end is what reads its channel to the end in the wait that reports it (@see Event); a
    /// shell's end leaves that read bounded.
    /// @param process The process, its role, and the pidfd its spawn opened, if one did.
    virtual void watchPid(WatchedProcess process) = 0;

    /// Reaps @p pid, watched and reported Ended. Called once per such process, after the last signal the
    /// keeper sends it.
    virtual void reap(int pid) = 0;

    /// Waits for anything to happen, or for @p deadline.
    /// @param deadline When to return with nothing (or a Timer event) at the latest.
    /// @return What happened, in order.
    [[nodiscard]] virtual std::vector<Event> wait(core::platform::SteadyTimePoint deadline) = 0;
};

/// The signals the keeper sends, by meaning; the adapter maps them to the platform's.
enum class KeeperSignal : uint8_t
{
    Terminate = 0, ///< Ask to stop (SIGTERM).
    Kill,          ///< End at once (SIGKILL).
    Abort,         ///< Abort with a core (SIGABRT): a hung daemon leaves evidence.
    Continue,      ///< Resume a stopped process (SIGCONT), so the abort lands.
};

/// Sends signals to the keeper's children.
class Signaller
{
  public:
    virtual ~Signaller() = default;

    /// @param pid The process. @param signal What to send.
    virtual void send(int pid, KeeperSignal signal) = 0;
};

/// How many more descriptors the keeper may open.
class DescriptorBudget
{
  public:
    virtual ~DescriptorBudget() = default;

    /// @return The descriptors this process may still open before its limit.
    [[nodiscard]] virtual std::size_t available() const = 0;
};

/// The ports the keeper runs on.
struct KeeperPorts
{
    DaemonLauncher& launcher;
    ShellSpawner& spawner;
    EventSource& events;
    Signaller& signaller;
    DescriptorBudget const& budget;
    core::platform::IClock const& clock;
};

} // namespace vthost::keeper
