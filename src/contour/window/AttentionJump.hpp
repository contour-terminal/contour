// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a listed pane asks of the user, and which pane "jump to the next item that needs you" goes to.
/// Qt-free and table-tested. For a pane whose flags the window keeps (FlagSource::Window), its urgency pools
/// what the directory states of it (an older daemon's records, output and bell, which stay set until this
/// window shows the pane) with this window's own ledger (an in-process pane's directory entry leaves them at
/// their defaults), so a mark from either source counts. For a pane whose flags a daemon keeps
/// (FlagSource::Daemon) the directory alone speaks: what it flags is unseen, for every window alike.

#include <contour/window/DaemonSection.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/PaneSeen.hpp>
#include <contour/window/ProgramStatusBadge.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/StatusUrgency.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace contour::window
{

/// How a finished command a daemon stated counts: a non-zero exit failed, exit 0 succeeded, and one whose
/// exit is unknown says nothing.
/// @param finished The command the directory states.
/// @return Its verdict.
[[nodiscard]] constexpr FinishedBlockBadge finishedBadgeOf(
    vthost::proto::FinishedCommand const& finished) noexcept
{
    if (!finished.exitCode)
        return FinishedBlockBadge::None;
    return *finished.exitCode == 0 ? FinishedBlockBadge::Succeeded : FinishedBlockBadge::Failed;
}

/// The serials a pane's directory entry carries, as TabAttention::noteSeen() takes them.
/// @param pane The directory's entry for the pane.
/// @return Its serials.
[[nodiscard]] constexpr TabAttention::FlagSerials serialsOf(DirectoryPane const& pane) noexcept
{
    return { .activity = pane.activitySerial, .bell = pane.bellSerial, .finished = pane.finishedSerial };
}

/// The newest program-status generation among @p pane's records.
/// @param pane The directory's entry for the pane.
/// @return The newest generation; 0 for none.
[[nodiscard]] inline uint64_t newestGenerationOf(DirectoryPane const& pane) noexcept
{
    auto newest = uint64_t { 0 };
    for (auto const& record: pane.status)
        newest = std::max(newest, record.generation);
    return newest;
}

/// What a window hands back to the daemon that keeps @p pane's flags once the user saw it: everything its
/// directory entry states so far.
/// @param pane The directory's entry for the pane.
/// @return Its serials and its newest program-status generation.
[[nodiscard]] inline PaneSeen paneSeenOf(DirectoryPane const& pane) noexcept
{
    return { .serials = serialsOf(pane), .statusGeneration = newestGenerationOf(pane) };
}

/// @param pane The directory's entry for a pane whose flags a daemon keeps.
/// @return Whether the daemon states something of it unseen: a flag, or a program status newer than seen;
///         never before it stated the pane at all (AttentionKnowledge::Unstated).
[[nodiscard]] inline bool isUnseen(DirectoryPane const& pane) noexcept
{
    if (pane.attention == AttentionKnowledge::Unstated)
        return false;
    return pane.activity == vthost::proto::OutputActivity::Output
           || pane.bell == vthost::proto::PaneBell::Rang || pane.finished.has_value()
           || newestGenerationOf(pane) > pane.seenStatusGeneration;
}

/// The seen program-status generation of a pane whose daemon stated nothing of it yet: every generation
/// counts as seen, so a Done or an Error it reports asks for nobody until the daemon says it was not seen.
inline constexpr auto AllStatusGenerationsSeen = std::numeric_limits<uint64_t>::max();

/// What a daemon states of a pane, as the marks a window's ledger would hold: each flag it sets is unseen.
/// A pane it stated nothing of yet holds no mark (AttentionKnowledge::Unstated).
/// @tparam Pane DirectoryPane, or the PaneStatus it is listed from: both carry the same flag fields.
/// @param pane The directory's entry, or the status, of a pane whose flags a daemon keeps.
/// @return Its marks.
template <typename Pane>
[[nodiscard]] constexpr SessionAttention daemonMarksOf(Pane const& pane) noexcept
{
    // Not stated yet: nothing of it is unseen, and every program status it reports counts as seen.
    if (pane.attention == AttentionKnowledge::Unstated)
        return SessionAttention { .seenStatusGeneration = AllStatusGenerationsSeen };
    return SessionAttention {
        .activity = pane.activity == vthost::proto::OutputActivity::Output ? TabActivity::UnseenOutput
                                                                           : TabActivity::None,
        .finished = pane.finished ? finishedBadgeOf(*pane.finished) : FinishedBlockBadge::None,
        .bell = pane.bell == vthost::proto::PaneBell::Rang ? BellMark::Rang : BellMark::None,
        .seenStatusGeneration = pane.seenStatusGeneration,
        .seenActivitySerial = 0,
        .seenBellSerial = 0,
        .seenFinishedSerial = 0,
    };
}

/// The marks a reader takes for a pane: the daemon's (daemonMarksOf()) where it keeps the pane's flags,
/// @p windowMarks otherwise.
/// @param pane The directory's entry for the pane.
/// @param windowMarks This window's own marks for it.
/// @return The marks that decide what the pane asks.
[[nodiscard]] constexpr SessionAttention marksOf(DirectoryPane const& pane,
                                                 SessionAttention const& windowMarks) noexcept
{
    switch (pane.flagSource)
    {
        case FlagSource::Window: return windowMarks;
        case FlagSource::Daemon: return daemonMarksOf(pane);
    }
    return windowMarks;
}

/// Notes in @p attention that the user sees @p pane: everything its directory entry states so far is seen.
/// @param attention This window's marks.
/// @param pane The directory's entry for the pane.
/// @return Changed when the marks changed.
[[nodiscard]] inline AttentionChange noteDirectoryPaneSeen(TabAttention& attention, DirectoryPane const& pane)
{
    return attention.noteSeen(pane.key, newestGenerationOf(pane), serialsOf(pane));
}

/// What a pane asks of the user.
/// @param pane        The directory's entry for the pane.
/// @param windowMarks This window's marks for the pane; ignored for a pane whose flags a daemon keeps.
/// @return The most urgent of: its program status (a seen Done or Error asks for nothing, a Blocked one
///         always asks), a bell, a finished command and output. Where the window keeps the flags, a flag a
///         daemon stated counts only while its serial is newer than the one the window has seen, and the
///         ledger's own marks count as they are; where the daemon keeps them, its flags and its seen status
///         generation alone decide (marksOf()).
[[nodiscard]] inline Urgency paneUrgency(DirectoryPane const& pane, SessionAttention const& windowMarks)
{
    auto const attention = marksOf(pane, windowMarks);
    auto const status = badgeFor(pane.status, attention.seenStatusGeneration);
    auto urgency = urgencyOf(status.kind, StatusSeen::Unseen);
    auto const rang =
        pane.bell == vthost::proto::PaneBell::Rang && pane.bellSerial > attention.seenBellSerial;
    if (rang || attention.bell == BellMark::Rang)
        urgency = std::max(urgency, urgencyOf(TabBadgeKind::Bell));
    auto finished = attention.finished;
    if (pane.finished && pane.finishedSerial > attention.seenFinishedSerial)
        finished = strongerFinishedBlock(finished, finishedBadgeOf(*pane.finished));
    switch (finished)
    {
        case FinishedBlockBadge::None: break;
        case FinishedBlockBadge::Succeeded: urgency = std::max(urgency, urgencyOf(TabBadgeKind::Done)); break;
        case FinishedBlockBadge::Failed: urgency = std::max(urgency, urgencyOf(TabBadgeKind::Error)); break;
    }
    auto const printed = pane.activity == vthost::proto::OutputActivity::Output
                         && pane.activitySerial > attention.seenActivitySerial;
    if (printed || attention.activity == TabActivity::UnseenOutput)
        urgency = std::max(urgency, urgencyOf(TabBadgeKind::Output));
    return urgency;
}

/// The urgencies a jump visits, most urgent first: the classes that ask something of the user. Working and
/// unseen output are not asking, so a jump never lands on them.
inline constexpr auto JumpClasses = std::array { Urgency::NeedsYou, Urgency::Failed, Urgency::DoneUnseen };

namespace detail
{
    /// The candidate a jump goes to.
    /// @param candidates Every pane with its urgency, in sidebar order.
    /// @param current    The index of the pane the jump starts from; none starts at the top.
    /// @return The first candidate after @p current, wrapping around, in the most urgent class of JumpClasses
    ///         that any candidate is in; none when no candidate is in any of them.
    template <typename Key>
    [[nodiscard]] std::optional<Key> nextInJumpClasses(std::vector<std::pair<Key, Urgency>> const& candidates,
                                                       std::optional<std::size_t> current)
    {
        auto const start = current ? std::next(candidates.begin(), static_cast<std::ptrdiff_t>(*current + 1))
                                   : candidates.begin();
        for (auto const wanted: JumpClasses)
        {
            auto const inClass = [wanted](auto const& candidate) {
                return candidate.second == wanted;
            };
            if (auto const found = std::find_if(start, candidates.end(), inClass); found != candidates.end())
                return found->first;
            if (auto const found = std::find_if(candidates.begin(), start, inClass); found != start)
                return found->first;
        }
        return std::nullopt;
    }

    /// @return The index of the first candidate @p matches accepts; none when it accepts none.
    template <typename Key, typename Matches>
    [[nodiscard]] std::optional<std::size_t> indexOf(std::vector<std::pair<Key, Urgency>> const& candidates,
                                                     Matches matches)
    {
        auto const at =
            std::ranges::find_if(candidates, [&](auto const& candidate) { return matches(candidate.first); });
        if (at == candidates.end())
            return std::nullopt;
        return static_cast<std::size_t>(std::distance(candidates.begin(), at));
    }
} // namespace detail

/// The pane a jump goes to.
/// @param directory The workspaces, tabs and panes, in sidebar order (depth-first).
/// @param attention This window's marks.
/// @param current   The pane the jump starts from; none starts at the top.
/// @return The first pane after @p current, wrapping around, in the most urgent class of JumpClasses that
///         any pane is in; none when no pane is in any of them.
[[nodiscard]] inline std::optional<PaneKey> nextAttention(WorkspaceDirectory const& directory,
                                                          TabAttention const& attention,
                                                          std::optional<PaneKey> current)
{
    auto panes = std::vector<std::pair<PaneKey, Urgency>> {};
    for (auto const& workspace: directory.workspaces)
        for (auto const& tab: workspace.tabs)
            for (auto const& pane: tab.panes)
                panes.emplace_back(pane.key, paneUrgency(pane, attention.of(pane.key)));
    auto const at =
        current ? detail::indexOf(panes, [&](PaneKey key) { return key == *current; }) : std::nullopt;
    return detail::nextInJumpClasses(panes, at);
}

/// The pane a jump goes to across every listed daemon, in tree order.
/// @param sections The window's sections (windowSections()), in tree order; a section that is not Connected,
///                 the window's own excepted, contributes nothing.
/// @param own      The window's own daemon; its panes are read with @p ownMarks (marksOf()).
/// @param ownMarks This window's marks.
/// @param current  Where the jump starts: the pending jump's target if one is pending, else the active pane.
///                 It is found by its daemon and pane; one that names no listed pane starts at the top.
/// @return As the single-directory overload, over all sections; a target names daemon, instance, workspace,
///         tab and pane.
[[nodiscard]] inline std::optional<SidebarTarget> nextAttention(std::span<DaemonSection const> sections,
                                                                DaemonRef const& own,
                                                                TabAttention const& ownMarks,
                                                                std::optional<SidebarTarget> const& current)
{
    auto panes = std::vector<std::pair<SidebarTarget, Urgency>> {};
    for (auto const& section: sections)
    {
        auto const isOwn = section.daemon == own;
        // A daemon this window does not reach now lists its last-known panes, which ask nothing of anyone.
        if (!isOwn && section.state != DaemonLinkState::Connected)
            continue;
        for (auto const& workspace: section.directory.workspaces)
            for (auto const& tab: workspace.tabs)
                for (auto const& pane: tab.panes)
                    panes.emplace_back(
                        SidebarTarget { .daemon = section.daemon,
                                        .instance = section.instance,
                                        .workspace = workspace.id,
                                        .tab = tab.id,
                                        .pane = pane.key },
                        paneUrgency(pane, isOwn ? ownMarks.of(pane.key) : SessionAttention {}));
    }
    auto const at = current && current->pane ? detail::indexOf(panes,
                                                               [&](SidebarTarget const& candidate) {
                                                                   return candidate.daemon == current->daemon
                                                                          && candidate.pane == current->pane;
                                                               })
                                             : std::nullopt;
    return detail::nextInJumpClasses(panes, at);
}

} // namespace contour::window
