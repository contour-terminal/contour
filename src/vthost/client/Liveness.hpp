// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// When a native connection is probed, and when it is given up on: a pure decision over time points, so a
/// half-open connection (a laptop that changed networks, a daemon host that vanished) is noticed in seconds
/// instead of whenever TCP next times out — and so every threshold is testable without a clock.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace vthost::client
{

/// How long a daemon may take to answer a ClientHello before the attempt is given up on: long enough for a
/// daemon busy restoring its sessions, short enough that a connection gone half-open mid-handshake is
/// retried instead of waited on for good.
inline constexpr auto DefaultHandshakeDeadline = std::chrono::milliseconds { 10000 };

/// The thresholds of the liveness decision.
struct LivenessPolicy
{
    /// How long the daemon may stay silent before it is sent a Heartbeat.
    std::chrono::milliseconds idleBeforeProbe { 5000 };
    /// How long it may stay silent before the connection is declared lost.
    std::chrono::milliseconds deadAfter { 15000 };
    /// How long it may take to answer the ClientHello. Applies to every daemon, whether or not it shares
    /// Capability::Heartbeat: an old daemon answers the hello too.
    std::chrono::milliseconds handshakeDeadline { DefaultHandshakeDeadline };
    /// A tick this much later than due means the process was frozen (sleep, VM pause, SIGSTOP).
    std::chrono::milliseconds awakeGap { 5000 };
    /// After a wake, how long the probe may go unanswered before the connection is lost.
    std::chrono::milliseconds wakeDeadline { 5000 };
};

/// What the connection should do now.
enum class LivenessAction : uint8_t
{
    None = 0,          ///< Nothing: the daemon spoke recently, a probe is already out, or the OS sleeps.
    SendHeartbeat = 1, ///< Probe the daemon: it has been silent for LivenessPolicy::idleBeforeProbe.
    DeclareLost = 2,   ///< Give up: it has been silent for LivenessPolicy::deadAfter.
};

/// Tracks when the daemon last spoke and whether a probe is out. Reads no clock: it is handed every time
/// point, by the connection's own loop clock.
class LivenessTracker
{
  public:
    /// @param policy The thresholds.
    explicit LivenessTracker(LivenessPolicy policy) noexcept: _policy { policy } {}

    /// Any frame from the daemon counts as life, an echo or not, and answers the probe out. It ends a wake
    /// state; it does not end a sleep.
    /// @param at When it arrived.
    void noteInbound(core::platform::SteadyTimePoint at) noexcept
    {
        _lastInbound = at;
        _probeOutstanding = ProbeState::None;
        if (_phase == Phase::Waking)
            _phase = Phase::Awake;
    }

    /// @param at When a Heartbeat was sent (unused but for symmetry with noteInbound: the silence is
    ///        measured from the last inbound frame, not from the probe).
    void noteProbeSent([[maybe_unused]] core::platform::SteadyTimePoint at) noexcept
    {
        _probeOutstanding = ProbeState::Outstanding;
    }

    /// The tick due at @p due ran at @p now: a late tick enters the wake state; an on-time tick
    /// awakeGap after noteSleep() leaves the sleep state (a sleep that was announced and never came).
    /// @param due When the tick was due.
    /// @param now When it ran.
    void noteTick(core::platform::SteadyTimePoint due, core::platform::SteadyTimePoint now) noexcept
    {
        if (now - due >= _policy.awakeGap
            || (_phase == Phase::Asleep && _asleepSince && now - *_asleepSince >= _policy.awakeGap))
            noteWake(now);
    }

    /// The OS announced a sleep: decide() returns None until a wake or the fallback of noteTick().
    /// @param at When the sleep was announced.
    void noteSleep(core::platform::SteadyTimePoint at) noexcept
    {
        _phase = Phase::Asleep;
        _asleepSince = at;
    }

    /// The OS announced a wake, or a late tick showed one: silence restarts at @p at, a probe is due at
    /// once, and wakeDeadline applies until the daemon speaks.
    /// @param at When the wake happened.
    void noteWake(core::platform::SteadyTimePoint at) noexcept
    {
        _phase = Phase::Waking;
        _asleepSince.reset();
        _lastInbound = at;
        _probeOutstanding = ProbeState::None;
    }

    /// @param now The current time.
    /// @return What to do now; None before anything arrived.
    [[nodiscard]] LivenessAction decide(core::platform::SteadyTimePoint now) const noexcept
    {
        if (!_lastInbound || _phase == Phase::Asleep)
            return LivenessAction::None;
        if (_phase == Phase::Waking && _probeOutstanding == ProbeState::None)
            return LivenessAction::SendHeartbeat;
        auto const silence = now - *_lastInbound;
        if (silence >= silenceLimit())
            return LivenessAction::DeclareLost;
        if (silence >= _policy.idleBeforeProbe && _probeOutstanding == ProbeState::None)
            return LivenessAction::SendHeartbeat;
        return LivenessAction::None;
    }

    /// @return The silence after which decide() says DeclareLost now: wakeDeadline while waking, deadAfter
    ///         otherwise. For the loss message.
    [[nodiscard]] std::chrono::milliseconds silenceLimit() const noexcept
    {
        return _phase == Phase::Waking ? _policy.wakeDeadline : _policy.deadAfter;
    }

    /// @return The thresholds.
    [[nodiscard]] LivenessPolicy const& policy() const noexcept { return _policy; }

  private:
    /// Whether a probe awaits its echo.
    enum class ProbeState : uint8_t
    {
        None = 0,        ///< No probe is out.
        Outstanding = 1, ///< One was sent since the daemon last spoke.
    };

    /// Whether the process is running normally, the OS sleeps, or it just woke and awaits the daemon.
    enum class Phase : uint8_t
    {
        Awake = 0,  ///< Normal operation.
        Asleep = 1, ///< The OS announced a sleep; no verdict until a wake.
        Waking = 2, ///< A wake was seen; the daemon has yet to speak.
    };

    LivenessPolicy _policy;
    std::optional<core::platform::SteadyTimePoint> _lastInbound;
    ProbeState _probeOutstanding = ProbeState::None;
    Phase _phase = Phase::Awake;
    std::optional<core::platform::SteadyTimePoint> _asleepSince;
};

} // namespace vthost::client
