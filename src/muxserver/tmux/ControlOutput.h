// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Control-mode output ordering: tmux's dual-queue design (control.c).
///
/// One "all" queue holds every block in arrival order — pane-output blocks and
/// notification lines; each pane additionally queues its own output blocks.
/// %output lines are emitted per-pane under a fair byte budget, while a
/// notification line is flushed ONLY once every output block queued before it
/// has been fully written. That is the ordering guarantee control-mode clients
/// rely on: a notification never appears between the %output lines of a block,
/// and never before output that preceded it.
///
/// Flow control mirrors tmux: when a pane's oldest unwritten output exceeds the
/// configured pause age, the pane is paused — its queued output is discarded
/// and a single %pause is emitted; %continue resumes it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace muxserver::tmux
{

/// Escapes raw pane bytes for a %output line: bytes < 0x20 and backslash become
/// `\ooo` (exactly three octal digits); 0x7F and all bytes >= 0x80 pass through
/// raw — byte-for-byte tmux's rule (control.c:664-675).
/// @param bytes The raw PTY output.
/// @return The escaped payload.
[[nodiscard]] std::string escapeOutput(std::string_view bytes);

/// tmux's flow-control constants (control.c:108-115).
struct ControlOutputLimits
{
    std::size_t bufferLow = 512;   ///< Below this, panes resume nothing special (reserved).
    std::size_t bufferHigh = 8192; ///< Target ceiling for the connection's buffered bytes.
    std::size_t writeMinimum = 32; ///< Floor for the per-pane byte budget per pass.
};

/// The dual-queue ordering core. Single-threaded (loop-confined); emission goes
/// through a caller-supplied sink (the connection's WriteQueue).
class ControlOutput
{
  public:
    /// @param sink Receives complete protocol lines, each including its LF.
    /// @param limits Flow-control constants (tests may shrink them).
    explicit ControlOutput(std::function<void(std::string)> sink, ControlOutputLimits limits = {});

    /// Queues a notification line (without LF). It flushes only after every
    /// pane-output block queued before it has been fully written.
    /// @param line The notification, e.g. `%window-add @1`.
    void enqueueNotification(std::string line);

    /// Queues raw pane output for %output emission.
    /// @param pane The pane id (the `%N` number).
    /// @param bytes The raw PTY bytes (unescaped).
    /// @param now The enqueue instant, for pause-age accounting.
    void enqueueOutput(std::uint64_t pane, std::string_view bytes, std::chrono::steady_clock::time_point now);

    /// Emits as much as the budget and ordering allow: leading notifications,
    /// then one fair pass of %output lines over panes with pending bytes, then
    /// any notifications those completions unblocked.
    /// @param buffered The connection's currently queued-but-unsent byte count
    ///        (the budget shrinks as the client falls behind).
    /// @param now The pump instant, for pause-age accounting.
    void pump(std::size_t buffered, std::chrono::steady_clock::time_point now);

    /// Enables tmux's pause-after flow control: a pane whose oldest unwritten
    /// output is older than @p age when pump() runs is paused (its pending
    /// output dropped, one `%pause %N` emitted). Disabled when nullopt.
    void setPauseAfter(std::optional<std::chrono::milliseconds> age) noexcept { _pauseAfter = age; }

    /// Switches emission to `%extended-output %N <age-ms> : <data>` — tmux uses
    /// it for every output line once the client set the pause-after flag
    /// (CLIENT_CONTROL_PAUSEAFTER, control.c:653-658), so the client can judge
    /// staleness itself.
    void setExtendedOutput(bool enabled) noexcept { _extendedOutput = enabled; }

    /// Resumes a paused pane, emitting `%continue %N` (refresh-client -A's job).
    /// @param pane The pane to resume.
    void continuePane(std::uint64_t pane);

    /// Force-pauses @p pane (refresh-client -A pane:pause): drops its pending
    /// output and emits `%pause %N` once. Idempotent while paused.
    void pausePane(std::uint64_t pane);

    /// Enables or disables forwarding for @p pane (refresh-client -A
    /// pane:on/off). Disabling drops pending output silently — unlike pausing
    /// there is no %pause/%continue handshake; the client asked for silence.
    void setPaneEnabled(std::uint64_t pane, bool enabled);

    /// @return True if @p pane is currently paused.
    [[nodiscard]] bool isPaused(std::uint64_t pane) const;

    /// @return The number of bytes queued for @p pane and not yet emitted.
    [[nodiscard]] std::size_t pendingBytes(std::uint64_t pane) const;

    /// @return True while any block (output or notification) awaits emission.
    [[nodiscard]] bool hasPending() const noexcept { return !_allBlocks.empty(); }

  private:
    /// One queued unit: a notification line, or a pane's output batch.
    struct Block
    {
        std::optional<std::uint64_t> pane;              ///< Set for output blocks.
        std::string data;                               ///< Notification line, or raw pane bytes.
        std::size_t offset = 0;                         ///< Bytes of an output block already emitted.
        std::chrono::steady_clock::time_point enqueued; ///< For pause-age accounting.
    };

    /// Flushes leading notification blocks (those not behind an unfinished
    /// output block) from the all-queue into the sink.
    void flushReadyNotifications();

    /// @return The panes that currently have unwritten output, in queue order.
    [[nodiscard]] std::vector<std::uint64_t> panesWithPending() const;

    /// Drops all of @p pane's pending output blocks (marking them done so the
    /// all-queue can advance past them).
    void discardPaneOutput(std::uint64_t pane);

    std::function<void(std::string)> _sink;
    ControlOutputLimits _limits;
    std::optional<std::chrono::milliseconds> _pauseAfter;
    bool _extendedOutput = false;

    /// The all-queue in arrival order — the single OWNER of every block
    /// (unique_ptr for pointer stability across deque operations). A block is
    /// destroyed only when the flush pops it off the front, after completion.
    std::deque<std::unique_ptr<Block>> _allBlocks;
    /// Per-pane views of the output blocks still being emitted (non-owning;
    /// entries are removed at completion, strictly before the all-queue pop).
    std::unordered_map<std::uint64_t, std::deque<Block*>> _paneQueues;
    std::unordered_map<std::uint64_t, bool> _paused;   ///< Pause state per pane.
    std::unordered_map<std::uint64_t, bool> _disabled; ///< -A pane:off state per pane.
};

} // namespace muxserver::tmux
