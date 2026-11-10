// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace vthost::keeper
{

/// The facts about one daemon process, advanced by the keeper as the process goes through its life.
/// One state per daemon process; classifyExit() reads the state the process ended in.
enum class DaemonEndState : uint8_t
{
    StartingFirst = 0, ///< The first daemon of this keeper, not yet serving.
    StartingRestart,   ///< A restarted daemon, not yet serving.
    Serving,           ///< The daemon announced that it serves.
    ShutdownAnnounced, ///< The daemon sent Shutdown.
    StopForwarded,     ///< The keeper forwarded a stop signal to the daemon.
    HungKilled,        ///< The keeper killed the daemon for missing its heartbeats.
    FirstStartKilled,  ///< The keeper killed the first daemon for not serving within the startup bound.
};

/// How a daemon process ended.
enum class DaemonExitKind : uint8_t
{
    FailedToStart = 0, ///< The first daemon never served.
    Crashed,           ///< It ended on its own, unannounced.
    Hung,              ///< The keeper killed it for silence.
    ShutDown,          ///< It was asked to, or announced that it would.
    HandedOff,         ///< It handed its sessions to a successor (live upgrade).
};

/// @param state The state the daemon process ended in.
/// @return How it ended. HandedOff is not decided here: only a live upgrade knows it.
[[nodiscard]] DaemonExitKind classifyExit(DaemonEndState state) noexcept;

/// The numbers of the crash-loop policy. They are tuning; the table the policy decides by is the contract.
struct RestartPolicyConfig
{
    std::chrono::milliseconds quickRepeat { 30'000 }; ///< A crash this soon after the last backs off.
    std::chrono::milliseconds backoffStart { 1'000 }; ///< The first backoff.
    std::chrono::milliseconds backoffCap { 30'000 };  ///< The backoff doubles up to this.
    int giveUpCrashes = 5;                            ///< This many crashes within giveUpWindow give up.
    std::chrono::milliseconds giveUpWindow { 300'000 };
    std::chrono::milliseconds grace { 120'000 }; ///< How long a given-up keeper waits before its one retry.
    std::chrono::milliseconds stableAfter { 300'000 }; ///< A daemon this long up clears the history.
    /// A starting daemon must say it is alive -- its first heartbeat, or Serving -- within this of its start,
    /// or is killed as hung (a first one as failed to start); from its first heartbeat on, this bounds each
    /// silence until it serves (or the heartbeat timeout, if longer).
    std::chrono::milliseconds restartStartupBound { 30'000 };
};

/// Where the policy stands in giving up.
enum class GiveUpPhase : uint8_t
{
    Running = 0, ///< Restarting as the table says.
    GivenUp,     ///< Fast restarts stopped; waiting out the grace period.
    Retrying,    ///< The one retry with the newest image runs.
};

/// What the keeper does about a daemon's end.
enum class RestartAction : uint8_t
{
    RestartNow = 0,   ///< Start the daemon again at once.
    RestartAfter,     ///< Start it again after RestartDecision::delay.
    GiveUp,           ///< Stop restarting; keep shells and listeners; call onGraceElapsed() after the delay.
    RetryNewestImage, ///< Start one more daemon, preferring a newer image.
    EndAll,           ///< End the shells, close the listeners, exit non-zero.
    ExitWithStatus,   ///< Exit with the daemon's own status.
    ExitClean,        ///< End the shells and exit zero.
    Ignore,           ///< Nothing: the timer or report is stale.
};

/// A decision of the policy.
struct RestartDecision
{
    RestartAction action = RestartAction::RestartNow;
    std::chrono::milliseconds delay {}; ///< For RestartAfter: the backoff. For GiveUp: the grace period.
};

/// The keeper's crash-loop policy: what to do, by table, when the daemon ends.
///
/// A first start that never served ends the keeper with the daemon's status. A crash restarts at once; a
/// crash soon after another backs off (doubling, capped); too many crashes in a window give up, wait out a
/// grace period, retry once, and a retry that crashes ends everything. A daemon that stays up clears it all.
class CrashRestartPolicy
{
  public:
    /// @param config The tuning numbers.
    explicit CrashRestartPolicy(RestartPolicyConfig config);

    /// The daemon process ended.
    /// @param kind How. @param now When.
    /// @return What to do.
    [[nodiscard]] RestartDecision onExit(DaemonExitKind kind, core::platform::SteadyTimePoint now);

    /// The grace period a GiveUp decision named has passed.
    /// @param now When.
    /// @return RetryNewestImage the first time, EndAll after, and Ignore when the policy has not given up
    ///         (a grace timer that outlived onStable()).
    [[nodiscard]] RestartDecision onGraceElapsed(core::platform::SteadyTimePoint now);

    /// The daemon has stayed up for RestartPolicyConfig::stableAfter: forget the crashes.
    /// @param now When.
    void onStable(core::platform::SteadyTimePoint now);

  private:
    RestartPolicyConfig _config;
    std::deque<core::platform::SteadyTimePoint> _crashes; ///< Within the give-up window.
    std::optional<core::platform::SteadyTimePoint> _lastCrash;
    std::chrono::milliseconds _backoff {}; ///< The last backoff handed out; zero when none is running.
    GiveUpPhase _phase = GiveUpPhase::Running;
};

} // namespace vthost::keeper
