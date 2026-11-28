// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's record of what each pane raised for the user's attention and what a window has seen of it.

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// What one pane raised for the user's attention, each flag stamped with the ledger serial that raised it
/// last.
struct PaneAttention
{
    /// Whether the pane printed output not yet seen.
    proto::OutputActivity activity = proto::OutputActivity::None;
    /// The serial that raised @ref activity last; 0 when it never was.
    uint64_t activitySerial = 0;
    /// Whether the pane rang a bell not yet seen.
    proto::PaneBell bell = proto::PaneBell::Silent;
    /// The serial of the latest bell; 0 when none rang.
    uint64_t bellSerial = 0;
    /// The command that finished unseen, if one did.
    std::optional<proto::FinishedCommand> finished = std::nullopt;
    /// The serial of the latest finish; 0 when none.
    uint64_t finishedSerial = 0;
    /// When @ref finished finished; absent with it.
    std::optional<std::chrono::system_clock::time_point> finishedAt = std::nullopt;
    /// The highest status generation a window reported seen.
    uint64_t seenStatusGeneration = 0;
    bool operator==(PaneAttention const&) const = default;
};

/// The serials a window has seen of one pane: the wire's spelling (MarkSeen), so the two never drift.
using SeenSerials = proto::SeenSerials;

/// Whether a ledger operation changed what the ledger states, so the caller knows whether to tell clients.
enum class AttentionChange : uint8_t
{
    Unchanged = 0, ///< The ledger states what it stated before.
    Changed,       ///< A pane's attention changed.
};

/// The daemon's attention ledger: per pane, the flags it raised (output, bell, a finished command), and one
/// serial counter shared by every kind, so a later raise always carries a higher serial than any earlier one.
/// A window acknowledges by serial (markSeen()), which clears only what it had seen: a bell rung after the
/// window looked rings again.
///
/// Pure state: it reads no clock (the caller passes a finish's time) and is owned by one thread.
class AttentionLedger
{
  public:
    AttentionLedger() = default;

    /// Rebuilds a ledger from a handover image.
    /// @param lastSerial The last serial the previous ledger handed out.
    /// @param panes Each pane's session and attention.
    /// @return A ledger stating @p panes, whose next serial follows @p lastSerial and every serial in @p
    /// panes.
    [[nodiscard]] static AttentionLedger restored(uint64_t lastSerial,
                                                  std::span<std::pair<uint64_t, PaneAttention> const> panes);

    /// Starts a quiet entry for @p session, so a window can mark its status seen before it raised anything.
    /// @param session The pane's session; one the ledger already has an entry for is left alone.
    void track(uint64_t session) { _panes.try_emplace(session); }

    /// Notes that @p session printed output. Only the first output since the last acknowledgement raises.
    /// @param session The pane's session.
    /// @return Changed when the pane went from no activity to activity.
    AttentionChange noteOutput(uint64_t session);

    /// Notes that @p session rang the bell. Every bell takes a new serial, so it rings past an earlier seen
    /// one.
    /// @param session The pane's session.
    /// @return Always Changed.
    AttentionChange noteBell(uint64_t session);

    /// Notes that a command of @p session finished; it replaces any earlier unseen one.
    /// @param session The pane's session.
    /// @param command How the command finished.
    /// @param at When it finished.
    /// @return Always Changed.
    AttentionChange noteFinished(uint64_t session,
                                 proto::FinishedCommand command,
                                 std::chrono::system_clock::time_point at);

    /// Acknowledges what a window saw of @p session: clears each flag whose serial is at most the one seen,
    /// and raises the pane's seen status generation (never lowers it).
    /// @param session The pane's session; an unknown one changes nothing.
    /// @param seen The serials the window has seen.
    /// @return Changed when a flag cleared or the seen status generation rose.
    AttentionChange markSeen(uint64_t session, SeenSerials seen);

    /// Drops @p session, whose pane is gone.
    /// @param session The pane's session.
    void forget(uint64_t session);

    /// @param session The pane's session.
    /// @return The pane's attention, or nullptr for a pane the ledger has no entry for.
    [[nodiscard]] PaneAttention const* of(uint64_t session) const;

    /// @return The last serial handed out; 0 before any.
    [[nodiscard]] uint64_t lastSerial() const noexcept { return _lastSerial; }

    /// @return Every pane's session and attention, by ascending session.
    [[nodiscard]] std::vector<std::pair<uint64_t, PaneAttention>> entries() const;

  private:
    /// @return The next serial, which is higher than every one handed out before.
    [[nodiscard]] uint64_t nextSerial() noexcept { return ++_lastSerial; }

    std::map<uint64_t, PaneAttention> _panes;
    uint64_t _lastSerial = 0;
};

/// @param session The pane's session.
/// @param attention What the ledger states of it.
/// @return The PaneAttentionState stating @p attention to a window.
[[nodiscard]] proto::PaneAttentionState attentionStateOf(uint64_t session, PaneAttention const& attention);

} // namespace vthost
