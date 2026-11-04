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
    /// A tick this much later than due means the process was frozen. Which freezes that reveals depends on
    /// the steady clock: on macOS it counts the time the system slept (libc++ reads CLOCK_MONOTONIC_RAW), so
    /// a sleep shows up as a late tick; on Linux and the BSDs it stops while the system is suspended, so a
    /// sleep leaves no gap to see -- and no false loss either, the silence not having grown. Everywhere it
    /// catches a SIGSTOP or a paused VM.
    std::chrono::milliseconds awakeGap { 5000 };
    /// After a wake, how long the connection may stay silent before it is lost while the wake's probe is
    /// unanswered: measured from the latest inbound frame, or from the wake when none arrived since. Stale
    /// frames the wake finds in the socket buffer drain within milliseconds, so a dead connection still runs
    /// out of silence about wakeDeadline after the wake; a live link busy with bulk output, whose echo is
    /// queued behind that output, keeps delivering and so keeps the deadline moving. deadAfter, measured
    /// from the wake, caps it: a link that delivers data but never the echo is not trusted forever.
    std::chrono::milliseconds wakeDeadline { 5000 };
    /// How long an announced sleep may stay untaken before liveness resumes as if it never came. Beyond the
    /// 30 s macOS (IOKit) gives an application to acknowledge a sleep, so a system still legitimately going
    /// to sleep is not mistaken for one that changed its mind.
    std::chrono::milliseconds announcedSleepGrace { 35000 };
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

    /// Any frame from the daemon counts as life, an echo or not, and answers the probe out -- except while
    /// waking: a frame read then may have been buffered before the sleep and says nothing about the
    /// connection now, so only noteProbeAnswered() ends a wake and the probe stays outstanding. It still
    /// restarts the wake's silence (@see LivenessPolicy::wakeDeadline): stale data is harmless because it
    /// drains within moments and silence then runs out as if it had not come. It does not end a sleep.
    /// @param at When it arrived.
    void noteInbound(core::platform::SteadyTimePoint at) noexcept
    {
        _lastInbound = at;
        if (_phase != Phase::Waking)
            _probeOutstanding = ProbeState::None;
    }

    /// The echo of the latest probe arrived: proof the connection carries traffic now, so it ends a wake
    /// state, and otherwise counts as any inbound frame does.
    /// @param at When it arrived.
    void noteProbeAnswered(core::platform::SteadyTimePoint at) noexcept
    {
        if (_phase == Phase::Waking)
            _phase = Phase::Awake;
        noteInbound(at);
    }

    /// @param at When a Heartbeat was sent (unused but for symmetry with noteInbound: the silence is
    ///        measured from the last inbound frame -- which a wake sets to the wake itself -- never from the
    ///        probe).
    void noteProbeSent([[maybe_unused]] core::platform::SteadyTimePoint at) noexcept
    {
        _probeOutstanding = ProbeState::Outstanding;
    }

    /// The tick due at @p due ran at @p now: a tick awakeGap late enters the wake state (@see
    /// LivenessPolicy::awakeGap for which freezes that reveals); an on-time tick announcedSleepGrace after
    /// noteSleep() leaves the sleep state (a sleep that was announced and never came).
    /// @param due When the tick was due.
    /// @param now When it ran.
    void noteTick(core::platform::SteadyTimePoint due, core::platform::SteadyTimePoint now) noexcept
    {
        if (now - due >= _policy.awakeGap
            || (_phase == Phase::Asleep && now - _asleepSince >= _policy.announcedSleepGrace))
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
    /// once, and wakeDeadline applies, measured from the latest inbound frame (from @p at when none arrives),
    /// until noteProbeAnswered() -- or until deadAfter has passed since @p at, however much arrived.
    /// @param at When the wake happened.
    void noteWake(core::platform::SteadyTimePoint at) noexcept
    {
        _phase = Phase::Waking;
        _wokeAt = at;
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
        if (exceededLimit(now))
            return LivenessAction::DeclareLost;
        if (now - *_lastInbound >= _policy.idleBeforeProbe && _probeOutstanding == ProbeState::None)
            return LivenessAction::SendHeartbeat;
        return LivenessAction::None;
    }

    /// @param now The current time.
    /// @return The limit after which decide() says DeclareLost: the one that ran out by @p now if one did --
    ///         deadAfter since a wake whose echo never came, whatever arrived meanwhile, or the silence limit
    ///         in force -- else the silence limit in force (wakeDeadline while waking, deadAfter otherwise).
    ///         For the loss message.
    [[nodiscard]] std::chrono::milliseconds lossLimit(core::platform::SteadyTimePoint now) const noexcept
    {
        return exceededLimit(now).value_or(silenceLimit());
    }

    /// @return The thresholds.
    [[nodiscard]] LivenessPolicy const& policy() const noexcept { return _policy; }

  private:
    /// @return The silence after which the connection is lost: wakeDeadline while waking, deadAfter
    /// otherwise.
    [[nodiscard]] std::chrono::milliseconds silenceLimit() const noexcept
    {
        return _phase == Phase::Waking ? _policy.wakeDeadline : _policy.deadAfter;
    }

    /// @param now The current time.
    /// @return The limit that ran out by @p now, if one did: deadAfter since the wake while waking (the cap,
    ///         checked first), else the silence limit since the latest inbound frame. None before anything
    ///         arrived.
    [[nodiscard]] std::optional<std::chrono::milliseconds> exceededLimit(
        core::platform::SteadyTimePoint now) const noexcept
    {
        if (!_lastInbound)
            return std::nullopt;
        if (_phase == Phase::Waking && now - _wokeAt >= _policy.deadAfter)
            return _policy.deadAfter;
        if (now - *_lastInbound >= silenceLimit())
            return silenceLimit();
        return std::nullopt;
    }

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
        Waking = 2, ///< A wake was seen; the daemon has yet to answer the wake's probe.
    };

    LivenessPolicy _policy;
    std::optional<core::platform::SteadyTimePoint> _lastInbound;
    ProbeState _probeOutstanding = ProbeState::None;
    Phase _phase = Phase::Awake;
    /// When the announced sleep began; meaningful only in Phase::Asleep, which is what says there is one.
    core::platform::SteadyTimePoint _asleepSince {};
    /// When the wake happened; meaningful only in Phase::Waking, where it anchors the deadAfter cap.
    core::platform::SteadyTimePoint _wokeAt {};
};

} // namespace vthost::client
