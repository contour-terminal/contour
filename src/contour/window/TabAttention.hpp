// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What each pane has to tell its tab's badge: output its user has not seen, and how its last unseen
/// command ended. Marked only while the pane is off screen, cleared once it is seen. Qt-free; every window
/// keeps one ledger of its own, keyed by PaneKey, and the decisions are table-tested.

#include <contour/platform/SessionVisibility.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/TabBadge.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <tuple>
#include <unordered_map>

#include <vthost/client/ConnectionState.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Primitives.hpp>
#include <vtworkspace/Tab.hpp>

namespace contour::window
{

/// Where a session's output comes from, as far as attention is concerned.
enum class OutputOrigin : uint8_t
{
    Live = 0, ///< The program wrote it.
    Resync,   ///< A mirror repainting what it already showed, around a reconnect: not news.
};

/// Whether a note changed what a session tells its tab.
enum class AttentionChange : uint8_t
{
    Unchanged = 0, ///< The badge inputs are as they were.
    Changed,       ///< The tab's badge must be read again.
};

/// What one session has to tell its tab.
struct SessionAttention
{
    TabActivity activity = TabActivity::None;               ///< Output the user has not seen.
    FinishedBlockBadge finished = FinishedBlockBadge::None; ///< The strongest unseen finished command.
    BellMark bell = BellMark::None;                         ///< A bell the user has not seen.
    uint64_t seenStatusGeneration = 0; ///< The newest program-status generation the user has seen.
    /// The newest serial of each flag a daemon stated for the pane that the user has seen
    /// (DirectoryPane::activitySerial and its siblings); a flag with a newer serial is news again.
    uint64_t seenActivitySerial = 0;
    uint64_t seenBellSerial = 0;     ///< @see seenActivitySerial
    uint64_t seenFinishedSerial = 0; ///< @see seenActivitySerial

    bool operator==(SessionAttention const&) const = default;
};

/// How long after a mirror's host connection returns output still counts as its resync. A resync repaints
/// every pane in as many chunks as the stream carries, so it is a span of time rather than one burst.
inline constexpr auto ResyncGracePeriod = std::chrono::milliseconds { 500 };

/// Where a mirrored session's output comes from.
/// @param connection Where the connection to the host stands.
/// @param now        The current time.
/// @param graceEnd   When the resync grace armed by the last return to Connected ends, if one was armed.
/// @return Resync while the connection is not up, or within the grace; Live otherwise.
[[nodiscard]] constexpr OutputOrigin outputOriginOf(
    vthost::client::ConnectionState connection,
    std::chrono::steady_clock::time_point now,
    std::optional<std::chrono::steady_clock::time_point> graceEnd)
{
    if (connection != vthost::client::ConnectionState::Connected)
        return OutputOrigin::Resync;
    if (graceEnd.has_value() && now < *graceEnd)
        return OutputOrigin::Resync;
    return OutputOrigin::Live;
}

/// The resync grace a connection change arms.
/// @param previous Where the connection stood.
/// @param next     Where it stands now.
/// @param now      The current time.
/// @return When the grace ends, for a return to Connected from any other state; nothing otherwise.
[[nodiscard]] constexpr std::optional<std::chrono::steady_clock::time_point> resyncGraceAfter(
    vthost::client::ConnectionState previous,
    vthost::client::ConnectionState next,
    std::chrono::steady_clock::time_point now)
{
    if (next == vthost::client::ConnectionState::Connected && previous != next)
        return now + ResyncGracePeriod;
    return std::nullopt;
}

/// The marks of every pane one window shows, by PaneKey. Each window keeps its own, so the same daemon
/// session shown in two windows is seen in one and still unseen in the other. Not thread-safe: the GUI
/// thread's alone.
///
/// Output is every frame the session's terminal reports, whatever its bytes said: an OSC 7501 status
/// report is a write like any other, so a background program that only reports its status marks unseen
/// output too. The badge shows that dot only once the program's status is None, as every status kind
/// outranks it (TabBadgeRules).
///
/// A remote pane's frame is output only when the mirror says so (vthost::client::frameOf): a repaint of
/// what the host showed already -- the attach, a resize, a resync snapshot -- is not. Output that overran
/// the server's scrollback floor between two pushes arrives as such a snapshot, so it reads as a resync:
/// a missed dot, never a false one.
class TabAttention
{
  public:
    /// Notes output in @p session.
    /// @param session    The session written to.
    /// @param visibility Where it is; only a Hidden session is marked.
    /// @param origin     Where the output came from; only Live output is news.
    /// @return Changed when the session was not marked before.
    [[nodiscard]] AttentionChange noteOutput(PaneKey session,
                                             platform::SessionVisibility visibility,
                                             OutputOrigin origin)
    {
        if (visibility != platform::SessionVisibility::Hidden || origin != OutputOrigin::Live)
            return AttentionChange::Unchanged;
        auto next = of(session);
        next.activity = TabActivity::UnseenOutput;
        return store(session, next);
    }

    /// Notes that a command in @p session finished.
    /// @param session    The session it ran in.
    /// @param visibility Where it is; only a Hidden session is marked.
    /// @param ending     How the command ended.
    /// @return Changed when the stronger of the held and the new verdict differs from the held one.
    [[nodiscard]] AttentionChange noteFinished(PaneKey session,
                                               platform::SessionVisibility visibility,
                                               vtbackend::CommandEnding ending)
    {
        if (visibility != platform::SessionVisibility::Hidden)
            return AttentionChange::Unchanged;
        auto next = of(session);
        next.finished = strongerFinishedBlock(next.finished, finishedBlockBadgeOf(ending));
        return store(session, next);
    }

    /// Notes that the bell rang in @p session.
    /// @param session    The session that rang it.
    /// @param visibility Where it is; only a Hidden session is marked.
    /// @param origin     Where the BEL came from; only Live output is news (a replay never badges).
    /// @return Changed when the session was not marked before.
    [[nodiscard]] AttentionChange noteBell(PaneKey session,
                                           platform::SessionVisibility visibility,
                                           OutputOrigin origin)
    {
        if (visibility != platform::SessionVisibility::Hidden || origin != OutputOrigin::Live)
            return AttentionChange::Unchanged;
        auto next = of(session);
        next.bell = BellMark::Rang;
        return store(session, next);
    }

    /// Notes that the user sees @p session: every mark clears, and the program-status records up to
    /// @p statusGeneration count as seen (Done and Error show only while newer). The seen generation only
    /// grows.
    /// @param session The session now on screen.
    /// @param statusGeneration The newest generation among the session's program-status records.
    /// @return Changed when it held a mark or the seen generation grew.
    [[nodiscard]] AttentionChange noteSeen(PaneKey session, uint64_t statusGeneration)
    {
        auto const held = of(session);
        auto next = held;
        next.activity = TabActivity::None;
        next.finished = FinishedBlockBadge::None;
        next.bell = BellMark::None;
        next.seenStatusGeneration = std::max(held.seenStatusGeneration, statusGeneration);
        return store(session, next);
    }

    /// The flag serials a daemon stated for one pane (DirectoryPane).
    struct FlagSerials
    {
        uint64_t activity = 0; ///< @see DirectoryPane::activitySerial
        uint64_t bell = 0;     ///< @see DirectoryPane::bellSerial
        uint64_t finished = 0; ///< @see DirectoryPane::finishedSerial
    };

    /// Notes that the user sees @p session, a pane listed from a daemon's directory: as noteSeen(), and the
    /// output, bell and finished command the daemon stated so far count as seen too.
    /// @param session The pane now seen.
    /// @param statusGeneration The newest generation among its program-status records.
    /// @param serials The serials its directory entry carries.
    /// @return Changed when it held a mark or any seen value grew.
    [[nodiscard]] AttentionChange noteSeen(PaneKey session, uint64_t statusGeneration, FlagSerials serials)
    {
        auto const first = noteSeen(session, statusGeneration);
        auto next = of(session);
        next.seenActivitySerial = std::max(next.seenActivitySerial, serials.activity);
        next.seenBellSerial = std::max(next.seenBellSerial, serials.bell);
        next.seenFinishedSerial = std::max(next.seenFinishedSerial, serials.finished);
        auto const second = store(session, next);
        return first == AttentionChange::Changed ? first : second;
    }

    /// Drops @p session's marks: it is gone.
    /// @param session The closed session.
    void forget(PaneKey session) { _marks.erase(session.value); }

    /// Holds @p marks as @p session's, as a pane moved here from another window's ledger brings them.
    /// @param session The pane that arrived.
    /// @param marks   What the ledger it left held for it.
    void adopt(PaneKey session, SessionAttention marks) { std::ignore = store(session, marks); }

    /// @param session The session asked about.
    /// @return Its marks; none for a session never marked.
    [[nodiscard]] SessionAttention of(PaneKey session) const
    {
        auto const found = _marks.find(session.value);
        return found != _marks.end() ? found->second : SessionAttention {};
    }

  private:
    /// Holds @p next as @p session's marks; an unmarked session holds no entry.
    [[nodiscard]] AttentionChange store(PaneKey session, SessionAttention next)
    {
        if (next == of(session))
            return AttentionChange::Unchanged;
        if (next == SessionAttention {})
            _marks.erase(session.value);
        else
            _marks.insert_or_assign(session.value, next);
        return AttentionChange::Changed;
    }

    std::unordered_map<uint64_t, SessionAttention> _marks; ///< Only marked sessions, by id.
};

/// What a tab's panes have to tell it, pooled.
/// @param tab    The tab.
/// @param ledger The marks of the window showing it.
/// @param keyOf  Resolves a leaf's session to the PaneKey the ledger holds it under.
/// @return Unseen output when any leaf has some, the strongest finished verdict of any leaf, and a bell when
///         any leaf rang.
template <typename KeyOf>
[[nodiscard]] SessionAttention pooledAttention(vtworkspace::Tab const& tab,
                                               TabAttention const& ledger,
                                               KeyOf const& keyOf)
{
    auto pooled = SessionAttention {};
    if (auto* root = tab.rootPane())
        root->walkTree([&](vtworkspace::Pane& pane) {
            if (!pane.isLeaf())
                return;
            auto const own = ledger.of(keyOf(pane.session()));
            if (own.activity == TabActivity::UnseenOutput)
                pooled.activity = TabActivity::UnseenOutput;
            pooled.finished = strongerFinishedBlock(pooled.finished, own.finished);
            if (own.bell == BellMark::Rang)
                pooled.bell = BellMark::Rang;
        });
    return pooled;
}

} // namespace contour::window
