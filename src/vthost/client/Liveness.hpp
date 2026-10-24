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
};

/// What the connection should do now.
enum class LivenessAction : uint8_t
{
    None = 0,          ///< Nothing: the daemon spoke recently, or a probe is already out.
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

    /// Any frame from the daemon counts as life, an echo or not, and answers the probe out.
    /// @param at When it arrived.
    void noteInbound(core::platform::SteadyTimePoint at) noexcept
    {
        _lastInbound = at;
        _probeOutstanding = ProbeState::None;
    }

    /// @param at When a Heartbeat was sent (unused but for symmetry with noteInbound: the silence is
    ///        measured from the last inbound frame, not from the probe).
    void noteProbeSent([[maybe_unused]] core::platform::SteadyTimePoint at) noexcept
    {
        _probeOutstanding = ProbeState::Outstanding;
    }

    /// @param now The current time.
    /// @return What to do now; None before anything arrived.
    [[nodiscard]] LivenessAction decide(core::platform::SteadyTimePoint now) const noexcept
    {
        if (!_lastInbound)
            return LivenessAction::None;
        auto const silence = now - *_lastInbound;
        if (silence >= _policy.deadAfter)
            return LivenessAction::DeclareLost;
        if (silence >= _policy.idleBeforeProbe && _probeOutstanding == ProbeState::None)
            return LivenessAction::SendHeartbeat;
        return LivenessAction::None;
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

    LivenessPolicy _policy;
    std::optional<core::platform::SteadyTimePoint> _lastInbound;
    ProbeState _probeOutstanding = ProbeState::None;
};

} // namespace vthost::client
