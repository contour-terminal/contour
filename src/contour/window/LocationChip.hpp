// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the location chip -- the one chip that names a window's daemon and workspace -- decides: the daemon's
/// name, whether its status dot shows, how the chip gives way as its room shrinks, and what assistive
/// technology and the tooltip say. Pure, so the chip, the switcher and the tests read the same rows.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/DaemonHosting.hpp>
#include <contour/window/EnumFromInt.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>
#include <libunicode/width.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/SocketPath.hpp>

// This header is Qt-free, yet its sentences are what `lupdate` collects for the "LocationChip" context: the
// marker is spelled as Qt spells it (same parameters, same expansion, so a translation unit that has Qt's own
// sees an identical redefinition) and does nothing here.
#ifndef QT_TRANSLATE_NOOP
    #define QT_TRANSLATE_NOOP(scope, x) x
#endif

namespace contour::window
{

/// @param address   Where the window's daemon was reached.
/// @param hostLabel The window's host label; empty for a local window.
/// @return What the chip calls the daemon: the host label when there is one, else "local" for the default
///         socket label, the label itself, or "daemon" when the address names none.
[[nodiscard]] inline std::string locationDaemonName(session::SwitchTarget const& address,
                                                    std::string_view hostLabel)
{
    if (!hostLabel.empty())
        return std::string(hostLabel);
    if (address.kind == vthost::proto::SwitchDaemonKind::Host && !address.daemon.empty())
        return address.daemon;
    if (address.kind == vthost::proto::SwitchDaemonKind::Label && !address.daemon.empty())
        return address.daemon == vthost::DefaultSocketLabel ? std::string("local") : address.daemon;
    return "daemon";
}

/// Whether the chip's status dot shows.
enum class DaemonDot : uint8_t
{
    Hidden = 0, ///< A local daemon that is up: nothing to say.
    Shown,      ///< A remote daemon, or a connection that is not up.
};

/// @param locality Where the daemon runs.
/// @param state    What the connection looks like.
/// @return Shown for a remote daemon or any state but Connected.
[[nodiscard]] constexpr DaemonDot daemonDotOf(DaemonLocality locality, HostChipState state) noexcept
{
    return locality == DaemonLocality::Remote || state != HostChipState::Connected ? DaemonDot::Shown
                                                                                   : DaemonDot::Hidden;
}

/// How much of the chip fits, from everything down to the workspace name alone.
enum class LocationChipFit : uint8_t
{
    Full = 0,        ///< Daemon, dot, state text and workspace.
    NoStateText,     ///< The state text is dropped.
    DaemonElided,    ///< The daemon name is elided too.
    DotOnly,         ///< The daemon shrinks to its dot.
    WorkspaceElided, ///< Not even that fits: the workspace name is elided.
};

/// Every @c LocationChipFit, the table its count comes from.
inline constexpr auto LocationChipFits = std::array { LocationChipFit::Full,
                                                      LocationChipFit::NoStateText,
                                                      LocationChipFit::DaemonElided,
                                                      LocationChipFit::DotOnly,
                                                      LocationChipFit::WorkspaceElided };
static_assert(enumValuesAreDense(LocationChipFits));

/// Number of @c LocationChipFit enumerators.
inline constexpr size_t LocationChipFitCount = LocationChipFits.size();

/// The least workspace cells the chip keeps before the `…`: it keeps whole grapheme clusters until they cover
/// that many, so six CJK characters (twelve cells) give three. The caller reserves that width, so the chip
/// never goes narrower.
inline constexpr std::size_t LocationChipWorkspaceFloor = 6;

/// One fit step: which fit it is and the key its width goes by.
struct LocationChipStep
{
    LocationChipFit fit;  ///< The step.
    std::string_view key; ///< The name QML measures the step's width by.
};

/// The steps in elision order: the chip takes the first whose width fits. The single source of that order and
/// of the width keys: @c locationChipFit and QML (through LocationChipModel::steps) both read this table, so
/// a new step is one row here. The last row is what is left when no other fits; its width is the least the
/// chip ever needs.
inline constexpr auto LocationChipSteps = std::array {
    LocationChipStep { .fit = LocationChipFit::Full, .key = "full" },
    LocationChipStep { .fit = LocationChipFit::NoStateText, .key = "noStateText" },
    LocationChipStep { .fit = LocationChipFit::DaemonElided, .key = "daemonElided" },
    LocationChipStep { .fit = LocationChipFit::DotOnly, .key = "dotOnly" },
    LocationChipStep { .fit = LocationChipFit::WorkspaceElided, .key = "workspaceElided" },
};
static_assert(LocationChipSteps.size() == LocationChipFitCount,
              "LocationChipSteps must have a row per LocationChipFit");

/// @param widthOf   Gives the width of a step by its key, in the units of @p available.
/// @param available The room the chip has.
/// @return The first step whose width fits, @c WorkspaceElided when none does.
template <typename WidthOf>
[[nodiscard]] constexpr LocationChipFit locationChipFit(WidthOf const& widthOf, double available)
{
    auto const fitting = std::ranges::find_if(
        LocationChipSteps, [&](LocationChipStep const& step) { return widthOf(step.key) <= available; });
    return fitting != LocationChipSteps.end() ? fitting->fit : LocationChipFit::WorkspaceElided;
}

/// The longest daemon name, in cells, the chip shows while it elides the name.
inline constexpr std::size_t LocationChipDaemonElidedMax = 10;

namespace detail
{
    /// One grapheme cluster of a text: its UTF-8 and the terminal cells it takes.
    struct Cluster
    {
        std::string text;  ///< The cluster, UTF-8.
        std::size_t cells; ///< Its width in cells (unicode::grapheme_cluster_width).
    };

    /// @param text UTF-8 text.
    /// @return The text's grapheme clusters in order, so an emoji ZWJ sequence or a base with its combining
    ///         marks is one piece that is never split.
    [[nodiscard]] inline std::vector<Cluster> clustersOf(std::string_view text)
    {
        auto clusters = std::vector<Cluster> {};
        for (std::u32string const& cluster: unicode::utf8_grapheme_segmenter(text))
            clusters.push_back(Cluster { .text = unicode::convert_to<char>(std::u32string_view { cluster }),
                                         .cells = unicode::grapheme_cluster_width(cluster) });
        return clusters;
    }

    /// @param clusters A text's clusters.
    /// @return The cells they take together.
    [[nodiscard]] inline std::size_t cellsOf(std::span<Cluster const> clusters) noexcept
    {
        return std::ranges::fold_left(
            clusters | std::views::transform(&Cluster::cells), std::size_t { 0 }, std::plus<> {});
    }

    /// @param clusters A text's clusters.
    /// @return The clusters' text joined.
    [[nodiscard]] inline std::string joined(std::span<Cluster const> clusters)
    {
        auto text = std::string {};
        for (auto const& cluster: clusters)
            text += cluster.text;
        return text;
    }

    /// @param clusters A text's clusters, from the end the piece is taken from.
    /// @param budget   The most cells the piece may take.
    /// @return How many clusters from the front of @p clusters fit in @p budget cells.
    [[nodiscard]] inline std::size_t countWithin(std::span<Cluster const> clusters,
                                                 std::size_t budget) noexcept
    {
        auto used = std::size_t { 0 };
        auto count = std::size_t { 0 };
        for (auto const& cluster: clusters)
        {
            if (used + cluster.cells > budget)
                break;
            used += cluster.cells;
            ++count;
        }
        return count;
    }
} // namespace detail

namespace detail
{
    /// @param daemon   The daemon's name; empty for an in-process window.
    /// @param clusters @p daemon's grapheme clusters.
    /// @param fit      How much of the chip fits.
    /// @return locationChipDaemonText() of a name that is already split.
    [[nodiscard]] inline std::string daemonTextOf(std::string_view daemon,
                                                  std::span<Cluster const> clusters,
                                                  LocationChipFit fit)
    {
        switch (fit)
        {
            case LocationChipFit::Full:
            case LocationChipFit::NoStateText: return std::string(daemon);
            case LocationChipFit::DaemonElided: {
                if (cellsOf(clusters) <= LocationChipDaemonElidedMax)
                    return std::string(daemon);
                auto const keep = LocationChipDaemonElidedMax - 1; // The ellipsis takes one cell.
                auto const head = countWithin(clusters, (keep + 1) / 2);
                auto const headCells = cellsOf(clusters.first(head));
                // The tail is what fits from the back, in the cells the head left of the budget.
                auto const reversed = std::vector<Cluster>(clusters.rbegin(), clusters.rend());
                auto const tail = countWithin(reversed, keep - headCells);
                return joined(clusters.first(head)) + "…" + joined(clusters.last(tail));
            }
            case LocationChipFit::DotOnly:
            case LocationChipFit::WorkspaceElided: return {};
        }
        return {};
    }

    /// @param workspace The workspace's name.
    /// @param clusters  @p workspace's grapheme clusters.
    /// @param fit       How much of the chip fits.
    /// @return locationChipWorkspaceText() of a name that is already split.
    [[nodiscard]] inline std::string workspaceTextOf(std::string_view workspace,
                                                     std::span<Cluster const> clusters,
                                                     LocationChipFit fit)
    {
        if (fit != LocationChipFit::WorkspaceElided)
            return std::string(workspace);
        auto kept = std::size_t { 0 };
        auto cells = std::size_t { 0 };
        for (auto const& cluster: clusters)
        {
            if (cells >= LocationChipWorkspaceFloor)
                break;
            cells += cluster.cells;
            ++kept;
        }
        if (kept >= clusters.size())
            return std::string(workspace);
        return joined(clusters.first(kept)) + "…";
    }
} // namespace detail

/// @param daemon The daemon's name; empty for an in-process window.
/// @param fit    How much of the chip fits.
/// @return The daemon segment's text: the whole name down to NoStateText, the name middle-elided to at most
///         LocationChipDaemonElidedMax cells at DaemonElided (whole grapheme clusters, the ellipsis counted),
///         nothing from DotOnly on.
[[nodiscard]] inline std::string locationChipDaemonText(std::string_view daemon, LocationChipFit fit)
{
    return detail::daemonTextOf(daemon, detail::clustersOf(daemon), fit);
}

/// Turns a source sentence into what the user reads: Qt's translation in the application, the sentence itself
/// where nothing translates it (the tests).
struct SourceText
{
    /// @param sentence A source-language sentence, with `%1`, `%2` for its arguments.
    /// @return The sentence, as it is.
    [[nodiscard]] std::string operator()(std::string_view sentence) const { return std::string(sentence); }
};

namespace detail
{
    /// @param sentence A (translated) sentence with `%1`, `%2`, ... for its arguments.
    /// @param arguments What each marker stands for, in order.
    /// @return The sentence with every marker replaced, in one pass, so an argument that looks like a marker
    ///         is left as it is.
    [[nodiscard]] inline std::string filled(std::string_view sentence,
                                            std::span<std::string_view const> arguments)
    {
        auto text = std::string {};
        for (auto at = size_t { 0 }; at < sentence.size(); ++at)
        {
            auto const digit = at + 1 < sentence.size() ? sentence[at + 1] - '0' : -1;
            if (sentence[at] == '%' && digit >= 1 && static_cast<size_t>(digit) <= arguments.size())
            {
                text += arguments[static_cast<size_t>(digit) - 1];
                ++at;
            }
            else
                text += sentence[at];
        }
        return text;
    }
} // namespace detail

/// @param state     What the connection looks like.
/// @param fit       How much of the chip fits.
/// @param translate Gives a source sentence's translation.
/// @return The state's words ("Reconnecting…") while the whole chip fits, else nothing; the tooltip then
///         carries them (locationChipToolTip()).
template <typename Translate = SourceText>
[[nodiscard]] std::string locationChipStatusText(HostChipState state,
                                                 LocationChipFit fit,
                                                 Translate const& translate = Translate {})
{
    auto const status = hostChipLook(state).status;
    return fit == LocationChipFit::Full && !status.empty() ? std::string(translate(status)) : std::string {};
}

/// @param workspace The workspace's name.
/// @param fit       How much of the chip fits.
/// @return The workspace's text: whole until the chip has to elide it, then whole grapheme clusters of it, at
///         least LocationChipWorkspaceFloor cells' worth (never fewer), and an ellipsis. A name the floor
///         already covers stays whole.
[[nodiscard]] inline std::string locationChipWorkspaceText(std::string_view workspace, LocationChipFit fit)
{
    return detail::workspaceTextOf(workspace, detail::clustersOf(workspace), fit);
}

/// Whether the switcher the chip opens is on screen.
enum class ChipDisclosure : uint8_t
{
    Collapsed = 0, ///< The switcher is closed.
    Expanded,      ///< The switcher is open.
};

/// What the chip says of one connection state beyond its dot.
struct LocationChipStateRow
{
    HostChipState state;            ///< The state this row describes.
    std::string_view tooltipFormat; ///< The tooltip's sentence (`%1` the daemon, `%2` the action); empty for
                                    ///< Connected, whose tooltip is the action alone.
    std::string_view description;   ///< What a screen reader adds to the name; empty for Connected.
    std::string_view glyph;         ///< The terminal-style stand-in for the dot.
};

/// The rows, indexed by @c HostChipState.
inline constexpr std::array<LocationChipStateRow, HostChipStateCount> LocationChipStateRows {
    LocationChipStateRow {
        .state = HostChipState::Connected, .tooltipFormat = "", .description = "", .glyph = "●" },
    LocationChipStateRow { .state = HostChipState::Reconnecting,
                           .tooltipFormat = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting to %1 · %2"),
                           .description = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting"),
                           .glyph = "◑" },
    LocationChipStateRow { .state = HostChipState::Disconnected,
                           .tooltipFormat = QT_TRANSLATE_NOOP("LocationChip", "Disconnected from %1 · %2"),
                           .description = QT_TRANSLATE_NOOP("LocationChip", "Disconnected"),
                           .glyph = "✕" },
    LocationChipStateRow {
        .state = HostChipState::NeedsAttention,
        .tooltipFormat = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting to %1 needs your attention · %2"),
        .description = QT_TRANSLATE_NOOP("LocationChip", "Reconnecting, needs your attention"),
        .glyph = "!" },
};

namespace detail
{
    [[nodiscard]] constexpr bool locationChipStateRowsInOrder() noexcept
    {
        for (auto const i: std::views::iota(size_t { 0 }, LocationChipStateRows.size()))
            if (LocationChipStateRows[i].state != HostChipState(i))
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::locationChipStateRowsInOrder(),
              "LocationChipStateRows must list the states in enumerator order");

/// What a screen reader adds when another workspace has news the marker shows, indexed by @c Urgency; empty
/// where the marker does not show.
inline constexpr std::array<std::string_view, 6> LocationChipOtherWorkspaceRows {
    "",                                                                // Idle
    "",                                                                // UnseenOutput
    "",                                                                // Working
    QT_TRANSLATE_NOOP("LocationChip", "Another workspace is done"),    // DoneUnseen
    QT_TRANSLATE_NOOP("LocationChip", "Another workspace has failed"), // Failed
    QT_TRANSLATE_NOOP("LocationChip", "Another workspace needs you"),  // NeedsYou
};

static_assert(static_cast<size_t>(Urgency::NeedsYou) + 1 == LocationChipOtherWorkspaceRows.size(),
              "LocationChipOtherWorkspaceRows must have a row per Urgency");

/// The words of one disclosure state, indexed by @c ChipDisclosure.
inline constexpr std::array<std::string_view, 2> LocationChipDisclosureRows {
    QT_TRANSLATE_NOOP("LocationChip", "collapsed"), QT_TRANSLATE_NOOP("LocationChip", "expanded")
};
static_assert(static_cast<size_t>(ChipDisclosure::Expanded) + 1 == LocationChipDisclosureRows.size());

/// What the chip tells a screen reader it is: what it shows, by the ui-ux rule that the name is what an item
/// shows and its status goes in the description.
/// @param daemon    The daemon's name; empty for an in-process window, which names no daemon.
/// @param workspace The workspace the window shows.
/// @param translate Gives a source sentence's translation.
/// @return `Workspace billing on starfive`, or `Workspace billing` for an in-process window.
template <typename Translate = SourceText>
[[nodiscard]] std::string locationChipAccessibleName(std::string_view daemon,
                                                     std::string_view workspace,
                                                     Translate const& translate = Translate {})
{
    if (daemon.empty())
        return detail::filled(translate(QT_TRANSLATE_NOOP("LocationChip", "Workspace %1")),
                              std::array<std::string_view, 1> { workspace });
    return detail::filled(translate(QT_TRANSLATE_NOOP("LocationChip", "Workspace %1 on %2")),
                          std::array<std::string_view, 2> { workspace, daemon });
}

/// @param daemon    The daemon's name.
/// @param translate Gives a source sentence's translation.
/// @return What a screen reader says of the control that names the daemon alone (the sidebar's head): `Daemon
///         local`, what it shows.
template <typename Translate = SourceText>
[[nodiscard]] std::string locationChipDaemonAccessibleName(std::string_view daemon,
                                                           Translate const& translate = Translate {})
{
    return detail::filled(translate(QT_TRANSLATE_NOOP("LocationChip", "Daemon %1")),
                          std::array<std::string_view, 1> { daemon });
}

/// @param state       What the connection looks like.
/// @param others      The most urgent roll-up of the workspaces the window does not show.
/// @param disclosure  Whether the switcher is open.
/// @param translate   Gives a source sentence's translation.
/// @return The chip's accessible description, its parts joined by commas: the connection's state when it is
///         not Connected, what another workspace asks while the marker shows, then the disclosure.
template <typename Translate = SourceText>
[[nodiscard]] std::string locationChipAccessibleDescription(HostChipState state,
                                                            Urgency others,
                                                            ChipDisclosure disclosure,
                                                            Translate const& translate = Translate {})
{
    auto parts = std::vector<std::string> {};
    if (auto const& word = LocationChipStateRows[static_cast<size_t>(state)].description; !word.empty())
        parts.emplace_back(translate(word));
    if (chipMarkerOf(others) == ChipMarker::Shown)
    {
        auto clause = std::string(translate(LocationChipOtherWorkspaceRows[static_cast<size_t>(others)]));
        // Mid-list the clause is no sentence of its own: "Reconnecting, another workspace has failed".
        if (!parts.empty() && !clause.empty())
            clause.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(clause.front())));
        parts.emplace_back(std::move(clause));
    }
    parts.emplace_back(translate(LocationChipDisclosureRows[static_cast<size_t>(disclosure)]));
    auto text = std::string {};
    for (auto const& part: parts)
        text += (text.empty() ? "" : ", ") + part;
    return text;
}

/// @param state What the connection looks like.
/// @return The glyph that stands for the state where a dot cannot be drawn.
[[nodiscard]] constexpr std::string_view daemonStateGlyph(HostChipState state) noexcept
{
    return LocationChipStateRows[static_cast<size_t>(state)].glyph;
}

/// @param state     What the connection looks like.
/// @param fit       How much of the chip fits; the state text is in the tooltip once it is elided.
/// @param daemon    The daemon's name; empty for an in-process window.
/// @param translate Gives a source sentence's translation.
/// @return The chip's tooltip: the action, preceded by the state while the chip does not show it itself.
template <typename Translate = SourceText>
[[nodiscard]] std::string locationChipToolTip(HostChipState state,
                                              LocationChipFit fit,
                                              std::string_view daemon,
                                              Translate const& translate = Translate {})
{
    auto action = std::string(
        translate(daemon.empty() ? QT_TRANSLATE_NOOP("LocationChip", "Switch workspace")
                                 : QT_TRANSLATE_NOOP("LocationChip", "Switch workspace or daemon")));
    auto const& row = LocationChipStateRows[static_cast<size_t>(state)];
    if (fit == LocationChipFit::Full || state == HostChipState::Connected || daemon.empty())
        return action;
    return detail::filled(std::string(translate(row.tooltipFormat)),
                          std::array<std::string_view, 2> { daemon, action });
}

/// What the chip says at one fit step, all of it: the texts QML lays out and the tooltip it hangs.
struct LocationChipStepText
{
    LocationChipFit fit;   ///< The step.
    std::string_view key;  ///< The name QML measures the step's width by (LocationChipSteps).
    std::string daemon;    ///< The daemon segment (locationChipDaemonText).
    std::string status;    ///< The state's words (locationChipStatusText).
    std::string workspace; ///< The workspace's text (locationChipWorkspaceText).
    std::string tooltip;   ///< The tooltip (locationChipToolTip).
};

/// The chip's texts at every fit step, in LocationChipSteps' order, splitting each name into grapheme
/// clusters once for all of them.
/// @param state     What the connection looks like.
/// @param daemon    The daemon's name; empty for an in-process window.
/// @param workspace The workspace's name.
/// @param translate Gives a source sentence's translation.
/// @return One row per step.
template <typename Translate = SourceText>
[[nodiscard]] std::vector<LocationChipStepText> locationChipStepTexts(
    HostChipState state,
    std::string_view daemon,
    std::string_view workspace,
    Translate const& translate = Translate {})
{
    auto const daemonClusters = detail::clustersOf(daemon);
    auto const workspaceClusters = detail::clustersOf(workspace);
    return LocationChipSteps | std::views::transform([&](LocationChipStep const& step) {
               return LocationChipStepText {
                   .fit = step.fit,
                   .key = step.key,
                   .daemon = detail::daemonTextOf(daemon, daemonClusters, step.fit),
                   .status = locationChipStatusText(state, step.fit, translate),
                   .workspace = detail::workspaceTextOf(workspace, workspaceClusters, step.fit),
                   .tooltip = locationChipToolTip(state, step.fit, daemon, translate),
               };
           })
           | std::ranges::to<std::vector>();
}

/// Where the location chip shows.
enum class LocationChipPlace : uint8_t
{
    None = 0, ///< Nowhere.
    Band,     ///< In the title band.
    Sidebar,  ///< In the workspace sidebar's header.
};

/// Whether the workspace sidebar is on screen.
enum class SidebarShown : uint8_t
{
    No = 0, ///< No sidebar.
    Yes,    ///< The sidebar shows.
};

/// @param band    Whether the title band shows.
/// @param centre  What the band's middle holds.
/// @param hosting Whether the sessions live in a daemon.
/// @param sidebar Whether the workspace sidebar shows.
/// @return Where the chip shows: a daemon's chip follows the band's centre, a window with no daemon
///         (in-process or attached to a tmux host) shows the chip only in the band's workspace-chip centre.
///         With no band, a daemon window's chip heads the sidebar when that shows, so the daemon is named
///         somewhere.
[[nodiscard]] constexpr LocationChipPlace locationChipPlace(BarPresence band,
                                                            BarCentre centre,
                                                            DaemonHosting hosting,
                                                            SidebarShown sidebar) noexcept
{
    if (band == BarPresence::Hidden)
        return hosting == DaemonHosting::Daemon && sidebar == SidebarShown::Yes ? LocationChipPlace::Sidebar
                                                                                : LocationChipPlace::None;
    switch (centre)
    {
        case BarCentre::Breadcrumb:
            return hosting == DaemonHosting::Daemon ? LocationChipPlace::Sidebar : LocationChipPlace::None;
        case BarCentre::ChipAndTabs: return LocationChipPlace::Band;
        case BarCentre::Empty:
        case BarCentre::Tabs:
        case BarCentre::Title:
            return hosting == DaemonHosting::Daemon ? LocationChipPlace::Band : LocationChipPlace::None;
    }
    return LocationChipPlace::None;
}

/// @param place Where the chip shows.
/// @return The word QML switches on: "none", "band" or "sidebar".
[[nodiscard]] constexpr std::string_view locationChipPlaceName(LocationChipPlace place) noexcept
{
    switch (place)
    {
        case LocationChipPlace::None: return "none";
        case LocationChipPlace::Band: return "band";
        case LocationChipPlace::Sidebar: return "sidebar";
    }
    return "none";
}

} // namespace contour::window
