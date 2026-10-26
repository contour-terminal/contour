// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What each session has to tell its tab's badge: output its user has not seen, and how its last unseen
/// command ended. Marked only while the session is off screen, cleared once it is seen. Qt-free and keyed
/// by session, so a session's marks follow it from window to window, and the decisions are table-tested.

#include <contour/platform/SessionVisibility.hpp>
#include <contour/window/TabBadge.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
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

/// The marks of every session, by id. Not thread-safe: the GUI thread's alone.
///
/// Output is every frame the session's terminal reports, whatever its bytes said: an OSC 7501 status
/// report is a write like any other, so a background program that only reports its status marks unseen
/// output too. The badge shows that dot only once the program's status is None, as every status kind
/// outranks it (TabBadgeRules).
class TabAttention
{
  public:
    /// Notes output in @p session.
    /// @param session    The session written to.
    /// @param visibility Where it is; only a Hidden session is marked.
    /// @param origin     Where the output came from; only Live output is news.
    /// @return Changed when the session was not marked before.
    [[nodiscard]] AttentionChange noteOutput(vtworkspace::SessionId session,
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
    [[nodiscard]] AttentionChange noteFinished(vtworkspace::SessionId session,
                                               platform::SessionVisibility visibility,
                                               vtbackend::CommandEnding ending)
    {
        if (visibility != platform::SessionVisibility::Hidden)
            return AttentionChange::Unchanged;
        auto next = of(session);
        next.finished = strongerFinishedBlock(next.finished, finishedBlockBadgeOf(ending));
        return store(session, next);
    }

    /// Notes that the user sees @p session: both marks clear.
    /// @param session The session now on screen.
    /// @return Changed when it held a mark.
    [[nodiscard]] AttentionChange noteSeen(vtworkspace::SessionId session)
    {
        return _marks.erase(session.value) != 0 ? AttentionChange::Changed : AttentionChange::Unchanged;
    }

    /// Drops @p session's marks: it is gone.
    /// @param session The closed session.
    void forget(vtworkspace::SessionId session) { _marks.erase(session.value); }

    /// @param session The session asked about.
    /// @return Its marks; none for a session never marked.
    [[nodiscard]] SessionAttention of(vtworkspace::SessionId session) const
    {
        auto const found = _marks.find(session.value);
        return found != _marks.end() ? found->second : SessionAttention {};
    }

  private:
    /// Holds @p next as @p session's marks; an unmarked session holds no entry.
    [[nodiscard]] AttentionChange store(vtworkspace::SessionId session, SessionAttention next)
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
/// @param ledger Every session's marks.
/// @return Unseen output when any leaf has some, and the strongest finished verdict of any leaf.
[[nodiscard]] inline SessionAttention pooledAttention(vtworkspace::Tab const& tab, TabAttention const& ledger)
{
    auto pooled = SessionAttention {};
    if (auto* root = tab.rootPane())
        root->walkTree([&](vtworkspace::Pane& pane) {
            if (!pane.isLeaf())
                return;
            auto const own = ledger.of(pane.session());
            if (own.activity == TabActivity::UnseenOutput)
                pooled.activity = TabActivity::UnseenOutput;
            pooled.finished = strongerFinishedBlock(pooled.finished, own.finished);
        });
    return pooled;
}

} // namespace contour::window
