// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The keystroke probe: types keys at the echo child and times each until a frame draws its echo.
///
/// A pure state machine over an injected clock. The frontend feeds it the cursor column of every
/// swapped frame and asks it for the next key to type; it never touches a window or a socket.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <vector>

#include <vthost/latency/EchoProtocol.hpp>

namespace vthost::latency
{

/// How a probe run goes.
struct ProbePlan
{
    std::size_t warmupKeys = 10;   ///< Keys typed first whose timings are dropped.
    std::size_t measuredKeys = 50; ///< Keys whose timings are kept.
    /// The shortest and the longest pause before a key; each gap is drawn from this range, which
    /// de-phases the key presses from the display's refresh.
    core::platform::SteadyDuration minGap = std::chrono::milliseconds { 30 };
    core::platform::SteadyDuration maxGap = std::chrono::milliseconds { 60 };
    /// How long the prompt may take to show, counted from the probe's construction.
    core::platform::SteadyDuration promptTimeout = std::chrono::seconds { 10 };
    /// How long an echo may take to show, counted from its key.
    core::platform::SteadyDuration echoTimeout = std::chrono::seconds { 2 };
    uint32_t seed = 42; ///< Seeds the gap draw, so a run's key schedule repeats.
};

/// Where the probe is.
enum class ProbePhase : uint8_t
{
    AwaitingPrompt = 0,
    Pausing,
    AwaitingEcho,
    Finished,
    PromptTimedOut,
    EchoTimedOut,
};

/// What a probe has measured so far.
struct ProbeResult
{
    ProbePhase phase = ProbePhase::AwaitingPrompt;
    /// When a frame first drew the prompt.
    std::optional<core::platform::SteadyTimePoint> firstPrompt;
    /// Key-to-pixel time of each measured key, in typing order.
    std::vector<core::platform::SteadyDuration> keystrokes;
    /// The cursor column the probe waited for when it timed out; -1 otherwise.
    int awaitedColumn = -1;
};

/// Drives one probe run. Thread-safe: the render thread calls frameSwapped(), the GUI thread
/// takeDueKey().
class KeystrokeProbe
{
  public:
    /// @param clock Where time is read from. Borrowed: it must outlive the probe.
    /// @param plan How the run goes.
    KeystrokeProbe(core::platform::IClock const& clock, ProbePlan plan);

    /// Reports a frame that was just swapped to the screen.
    /// @param drawnCursorColumn The 0-based column the frame drew the cursor at, or nullopt if it
    ///        drew none.
    void frameSwapped(std::optional<int> drawnCursorColumn);

    /// @return The key to type now, if its pause has elapsed; nullopt otherwise. Also settles the
    ///         timeouts.
    [[nodiscard]] std::optional<char> takeDueKey();

    /// @return The phase the probe is in.
    [[nodiscard]] ProbePhase phase() const;

    /// @return What has been measured so far.
    [[nodiscard]] ProbeResult result() const;

  private:
    [[nodiscard]] core::platform::SteadyDuration drawGap();
    void pauseFrom(core::platform::SteadyTimePoint now);

    core::platform::IClock const& _clock;
    ProbePlan _plan;
    mutable std::mutex _mutex;
    std::mt19937 _random;
    core::platform::SteadyTimePoint _startedAt;
    core::platform::SteadyTimePoint _dueAt {};
    core::platform::SteadyTimePoint _keyTypedAt {};
    std::size_t _keyIndex = 0;
    ProbeResult _result;
};

} // namespace vthost::latency
