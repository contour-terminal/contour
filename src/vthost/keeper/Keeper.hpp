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

/// How long a daemon that announced Shutdown, or was forwarded a stop, has to exit before it is killed;
/// below systemd's default stop timeout, so the keeper still ends the shells itself.
inline constexpr std::chrono::milliseconds DaemonExitBound { 60'000 };

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
    /// @param policy        The crash-loop policy's numbers.
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
        UniqueFd fd;
    };

    [[nodiscard]] core::platform::SteadyTimePoint now() const noexcept { return _ports.clock.now(); }

    void launchDaemon(LaunchContinuity continuity);
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
    void handle(Listener const& message, std::vector<UniqueFd>& fds);
    void handle(Serving const& message, std::vector<UniqueFd>& fds);
    void handle(Spawn const& message, std::vector<UniqueFd>& fds);
    void handle(Placed const& message, std::vector<UniqueFd>& fds);
    void handle(Release const& message, std::vector<UniqueFd>& fds);
    void handle(Structure const& message, std::vector<UniqueFd>& fds);
    void handle(Heartbeat const& message, std::vector<UniqueFd>& fds);
    void handle(Adopt const& message, std::vector<UniqueFd>& fds);
    void handle(Shutdown const& message, std::vector<UniqueFd>& fds);
    void handle(auto const& /*message*/, std::vector<UniqueFd>& /*fds*/) {}

    KeeperPorts _ports;
    std::unique_ptr<LabelLock> _labelLock;
    RestartPolicyConfig _policyConfig;
    CrashRestartPolicy _policy;
    std::filesystem::path _daemonImage;
    std::vector<uint8_t> _launchOptions;

    SessionTable _sessions;
    std::vector<KeptListener> _listeners;
    UniqueFd _structure;
    std::optional<Daemon> _daemon;
    uint64_t _lastGeneration = 0;
    std::optional<core::platform::SteadyTimePoint> _relaunchAt; ///< A backed-off restart.
    std::optional<core::platform::SteadyTimePoint> _graceAt;    ///< The end of a give-up's grace period.
    std::optional<KeeperOutcome> _ending;                       ///< Set once the keeper began to end.
    std::optional<KeeperOutcome> _outcome;                      ///< Set once it ended.
    int _exitStatus = 0;
};

} // namespace vthost::keeper
