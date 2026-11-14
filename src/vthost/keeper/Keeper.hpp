// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/Ports.hpp>
#include <vthost/keeper/RestartPolicy.hpp>
#include <vthost/keeper/SessionTable.hpp>
#include <vthost/keeper/UniqueFd.hpp>

namespace vthost::keeper
{

/// How long a daemon the keeper aborted for silence has to end before it is killed.
inline constexpr std::chrono::milliseconds HungKillDelay { 10'000 };

/// How long the shells have to end after the keeper hung them up at its end, before they are killed.
inline constexpr std::chrono::milliseconds ShellHangupGrace { 3'000 };

/// How long the keeper waits, after a daemon was reaped, for its channel's last frames and close before it
/// judges the exit anyway (the event source reports them first; @see EventSource).
inline constexpr std::chrono::milliseconds ChannelDrainBound { 2'000 };

/// The shortest heartbeat timeout the keeper accepts from Serving; a shorter one is raised to it.
inline constexpr std::chrono::milliseconds MinHeartbeatTimeout { 1'000 };

/// How long a starting daemon's heartbeats may stop before Serving brings its own timeout: Serving's default.
inline constexpr std::chrono::milliseconds DefaultHeartbeatTimeout { Serving {}.heartbeatTimeoutMs };

/// The descriptors a spawn must leave free; a spawn that would leave fewer is refused.
inline constexpr std::size_t SpawnDescriptorReserve = 64;

/// How the keeper ended.
enum class KeeperOutcome : uint8_t
{
    Clean = 0,           ///< The daemon shut down, or the keeper was stopped; the shells were ended.
    DaemonFailedToStart, ///< The first daemon never served; the keeper exits with its status.
    GaveUp,              ///< The daemon crashed too often; everything was ended.
};

/// The PTY keeper: the parent of the daemon and of every shell. It holds a duplicate of every PTY master
/// and listener, restarts a daemon that crashed or hung, and hands what it holds to the restarted one.
///
/// Single-threaded: run() waits on the EventSource and handles what it reports, nothing else runs.
/// Everything it reaches of the system is a port (@see KeeperPorts), so the whole state machine runs
/// against fakes.
class Keeper
{
  public:
    /// @param ports         The system, as ports; they outlive the keeper.
    /// @param labelLock     The lock on the label, held until the daemon announces Shutdown or the keeper
    /// ends.
    /// @param policy        The crash-loop policy's numbers, and how long a stopped daemon has to exit.
    /// @param daemonImage   The daemon executable to start, and to restart unless a newer one is named.
    /// @param launchOptions The daemon's options record, opaque to the keeper, sent with every Launch.
    Keeper(KeeperPorts ports,
           std::unique_ptr<LabelLock> labelLock,
           RestartPolicyConfig policy,
           std::filesystem::path daemonImage,
           std::vector<uint8_t> launchOptions);

    /// Starts the daemon and keeps it until the keeper ends. Called once.
    /// @return How the keeper ended.
    [[nodiscard]] KeeperOutcome run();

    /// @return The status the keeper process exits with: 0 when clean, the daemon's own status when its
    ///         first start failed, non-zero when it gave up.
    [[nodiscard]] int exitStatus() const noexcept { return _exitStatus; }

    /// @return The channel version the keeper and the daemon's current image both speak, as negotiated
    ///         from that image's Hello: what gates a message, enum value or descriptor-carrying field a later
    ///         version adds (@see Messages.hpp). 0 while no image has said Hello.
    [[nodiscard]] uint32_t daemonChannelVersion() const noexcept
    {
        return _daemon ? _daemon->channelVersion : 0;
    }

  private:
    /// Whether the daemon has received the inventory it is owed.
    enum class InventoryDelivery : uint8_t
    {
        Owed = 0,  ///< A recovered daemon that has not sent Adopt yet.
        Delivered, ///< A fresh daemon, or one that adopted: tell it of exits as they happen.
    };

    /// Whether a heartbeat came from the daemon process yet: until one does, its start is bounded by the
    /// restart policy's startup bound, and by its heartbeats from then on.
    enum class Beats : uint8_t
    {
        NoneYet = 0,
        Heard,
    };

    /// Whether the daemon's image has said Hello yet.
    enum class Greeting : uint8_t
    {
        Awaited = 0, ///< Launched; its first image has not spoken.
        Greeted,     ///< It said Hello: another Hello is a new image, exec'd in place.
    };

    /// Whether the keeper writes to the daemon now, or holds what it would write back.
    enum class Writes : uint8_t
    {
        Sent = 0, ///< Frames go to the daemon as they are made.
        /// The daemon announced an exec (Exec, answered ExecNoted): frames are held back until its new
        /// image's Hello or ExecAborted, so that none is half-read by the image the exec ends.
        HeldBack,
    };

    /// The image a daemon announced it is about to exec (Exec), held until that image says Hello: an exec
    /// that fails leaves the daemon on its old image, and the keeper restarting the image it ran.
    struct ExecTarget
    {
        std::filesystem::path executable;
        std::unique_ptr<HeldImage> held;
    };

    struct Daemon
    {
        int pid = 0;
        DaemonGeneration generation {};
        std::unique_ptr<Channel> channel; ///< None once it closed.
        DaemonEndState state = DaemonEndState::StartingFirst;
        InventoryDelivery inventory = InventoryDelivery::Delivered;
        core::platform::SteadyTimePoint startedAt {};
        HungRestart hungRestart = HungRestart::Off; ///< From Serving.
        /// From Serving; DefaultHeartbeatTimeout before it, for a start that beats.
        std::chrono::milliseconds heartbeatTimeout = DefaultHeartbeatTimeout;
        Beats beats = Beats::NoneYet;
        core::platform::SteadyTimePoint lastHeartbeat {};
        std::optional<core::platform::SteadyTimePoint> killAt;
        std::optional<core::platform::SteadyTimePoint> stableAt;
        std::optional<WireExitStatus> exited; ///< Reaped; judged once the channel closed.
        std::optional<core::platform::SteadyTimePoint>
            drainUntil; ///< Judged by then if the close never comes.
        Greeting greeting = Greeting::Awaited;
        uint32_t channelVersion = 0;          ///< Negotiated from its current image's Hello; 0 before one.
        std::optional<ExecTarget> execTarget; ///< Announced by Exec; committed by the new image's Hello.
        Writes writes = Writes::Sent;
        std::vector<Message> heldBack; ///< What is held back while Writes::HeldBack, in order.
    };

    /// A session spawnSession() started.
    struct SpawnedSession
    {
        KeeperSessionId id {};
        int pid = 0;
        int processHandle = -1; ///< Its pidfd, owned by the session table; -1 for none.
    };

    struct KeptListener
    {
        std::string name;
        std::optional<std::vector<uint8_t>> tlsIdentity;
        std::string path; ///< The socket file of a Unix listener; empty for TCP.
        UniqueFd fd;
    };

    /// Whether a daemon process was started.
    enum class Launched : uint8_t
    {
        No = 0,
        Yes,
    };

    [[nodiscard]] core::platform::SteadyTimePoint now() const noexcept { return _ports.clock.now(); }

    [[nodiscard]] Launched launchDaemon(LaunchContinuity continuity);
    void restart(); ///< Starts a Recovered daemon, counting each launch that fails as a crash.
    void onEvent(Event event);
    void onFrame(Frame frame);
    void onEnded(int pid, WireExitStatus status);
    void onLinkLost();
    void judgeDaemonExit();
    void onStopSignal();
    /// Measures the daemon's silence afresh when the wait for @p deadline returned more than
    /// MinHeartbeatTimeout late: the keeper itself was stopped, not the daemon hung.
    void forgiveOwnStop(core::platform::SteadyTimePoint deadline);
    void onTimers();
    void apply(RestartDecision decision, WireExitStatus status);
    void beginEnding(KeeperOutcome outcome, int exitStatus);
    /// Lets go of the label and of every deposited listener, removing their socket files first: under the
    /// lock, no keeper that starts on the label after it can have bound a path yet. What every ending of the
    /// daemon does, for a daemon behind a keeper never removes them itself (SocketFileRemoval::LeftToKeeper).
    void releaseLabelAndListeners();
    void dropChannel();
    void send(Message const& message, std::vector<int> fds = {});
    [[nodiscard]] bool linked() const noexcept;
    [[nodiscard]] bool takesIntake() const noexcept;
    [[nodiscard]] bool hasEnded() const noexcept; ///< Ending, with no daemon and no shell left.
    [[nodiscard]] std::expected<SpawnedSession, SpawnError> spawnSession(Spawn const& message,
                                                                         std::vector<UniqueFd>& fds);
    [[nodiscard]] std::optional<core::platform::SteadyTimePoint> silenceDeadline() const;
    [[nodiscard]] core::platform::SteadyTimePoint nextDeadline() const;

    // One handler per message the daemon sends the keeper; the rest are ignored.
    /// Takes the hung-daemon settings of @p message: on Serving, and again from a live upgrade's new image.
    void adoptKeepingSettings(Daemon& daemon, Serving const& message);

    void handle(Hello const& message, std::vector<UniqueFd>& fds);
    void handle(Listener const& message, std::vector<UniqueFd>& fds);
    void handle(Serving const& message, std::vector<UniqueFd>& fds);
    void handle(Spawn const& message, std::vector<UniqueFd>& fds);
    void handle(Placed const& message, std::vector<UniqueFd>& fds);
    void handle(Release const& message, std::vector<UniqueFd>& fds);
    void handle(Structure const& message, std::vector<UniqueFd>& fds);
    void handle(Heartbeat const& message, std::vector<UniqueFd>& fds);
    void handle(Adopt const& message, std::vector<UniqueFd>& fds);
    void handle(Shutdown const& message, std::vector<UniqueFd>& fds);
    void handle(Signal const& message, std::vector<UniqueFd>& fds);
    void handle(Exec const& message, std::vector<UniqueFd>& fds);
    void handle(ExecAborted const& message, std::vector<UniqueFd>& fds);
    /// Sends what was held back while the daemon execs, in order, and sends as frames are made again.
    void releaseHeldBack();
    void handle(auto const& /*message*/, std::vector<UniqueFd>& /*fds*/) {}

    KeeperPorts _ports;
    std::unique_ptr<LabelLock> _labelLock;
    CrashRestartPolicy _policy;
    std::filesystem::path _daemonImage; ///< The committed image: what a crashed daemon is restarted from.
    /// The hold on @ref _daemonImage once a live upgrade moved it there; null for the image the keeper was
    /// started with, which the keeper process's own lease holds.
    std::unique_ptr<HeldImage> _daemonImageHold;
    std::vector<uint8_t> _launchOptions;

    SessionTable _sessions;
    /// What DescriptorBudget::available() said in this wait's batch of events, less the descriptor each spawn
    /// of the batch kept since; asked again in the next batch. The budget counts what the process holds, from
    /// wherever it came -- received with frames, opened for a spawn, inherited -- which no running count here
    /// could follow exactly, so it is asked at most once a batch rather than once a spawn.
    std::optional<std::size_t> _descriptorsLeft;
    std::vector<KeptListener> _listeners;
    UniqueFd _structure;
    std::optional<Daemon> _daemon;
    uint64_t _lastGeneration = 0;
    std::optional<core::platform::SteadyTimePoint> _relaunchAt; ///< A backed-off restart.
    std::optional<core::platform::SteadyTimePoint> _graceAt;    ///< The end of a give-up's grace period.
    std::optional<KeeperOutcome> _ending;                       ///< Set once the keeper began to end.
    int _exitStatus = 0;
};

} // namespace vthost::keeper
