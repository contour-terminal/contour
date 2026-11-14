// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the location chip -- the one chip that names a window's daemon and workspace -- decides: the daemon's
/// name, whether its status dot shows, how the chip gives way as its room shrinks, and what assistive
/// technology and the tooltip say. Pure, so the chip, the switcher and the tests read the same rows.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/DaemonHosting.hpp>
#include <contour/window/DisclosureWords.hpp>
#include <contour/window/EnumFromInt.hpp>
#include <contour/window/GraphemeElide.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/TranslateNoop.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
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

/// Which chips a fit step is one of: the daemon-only control never shrinks past its name.
enum class ChipStepUse : uint8_t
{
    WithWorkspaceOnly =
        0,         ///< A step of the chip that names the workspace too: it exists to shrink the daemon.
    DaemonOnlyToo, ///< A step the daemon-only control takes as well, since it still names the daemon.
};

/// One fit step: which fit it is, the key its width goes by and which chips take it.
struct LocationChipStep
{
    LocationChipFit fit;  ///< The step.
    std::string_view key; ///< The name QML measures the step's width by.
    ChipStepUse use;      ///< Which chips take the step.
};

/// The steps in elision order: the chip takes the first whose width fits. The single source of that order and
/// of the width keys: @c locationChipFit and QML (through LocationChipModel::steps) both read this table, so
/// a new step is one row here. The last row is what is left when no other fits; its width is the least the
/// chip ever needs. The daemon-only control (`local ▾`) keeps its name: DotOnly and WorkspaceElided would
/// leave it a bare caret, so it takes neither, and its last step is DaemonElided.
inline constexpr auto LocationChipSteps = std::array {
    LocationChipStep { .fit = LocationChipFit::Full, .key = "full", .use = ChipStepUse::DaemonOnlyToo },
    LocationChipStep {
        .fit = LocationChipFit::NoStateText, .key = "noStateText", .use = ChipStepUse::DaemonOnlyToo },
    LocationChipStep {
        .fit = LocationChipFit::DaemonElided, .key = "daemonElided", .use = ChipStepUse::DaemonOnlyToo },
    LocationChipStep {
        .fit = LocationChipFit::DotOnly, .key = "dotOnly", .use = ChipStepUse::WithWorkspaceOnly },
    LocationChipStep { .fit = LocationChipFit::WorkspaceElided,
                       .key = "workspaceElided",
                       .use = ChipStepUse::WithWorkspaceOnly },
};
static_assert(LocationChipSteps.size() == LocationChipFitCount,
              "LocationChipSteps must have a row per LocationChipFit");

/// @param step     A fit step.
/// @param segments What the chip names.
/// @return Whether a chip that names @p segments takes @p step.
[[nodiscard]] constexpr bool locationChipTakes(LocationChipStep const& step, ChipSegments segments) noexcept
{
    return segments == ChipSegments::DaemonAndWorkspace || step.use == ChipStepUse::DaemonOnlyToo;
}

/// @param segments What the chip names.
/// @return The last step such a chip takes: the least it ever shows, what is left when no other fits.
[[nodiscard]] constexpr LocationChipFit locationChipLeastFit(
    ChipSegments segments = ChipSegments::DaemonAndWorkspace) noexcept
{
    auto const taken = [&](LocationChipStep const& step) {
        return locationChipTakes(step, segments);
    };
    auto const last = std::ranges::find_if(LocationChipSteps | std::views::reverse, taken);
    return (*last).fit;
}

/// @param widthOf   Gives the width of a step by its key, in the units of @p available.
/// @param available The room the chip has.
/// @param segments  What the chip names: the daemon-only control takes fewer steps.
/// @return The first step the chip takes whose width fits, its least step when none does.
template <typename WidthOf>
[[nodiscard]] constexpr LocationChipFit locationChipFit(
    WidthOf const& widthOf, double available, ChipSegments segments = ChipSegments::DaemonAndWorkspace)
{
    auto const fits = [&](LocationChipStep const& step) {
        return locationChipTakes(step, segments) && widthOf(step.key) <= available;
    };
    auto const fitting = std::ranges::find_if(LocationChipSteps, fits);
    return fitting != LocationChipSteps.end() ? fitting->fit : locationChipLeastFit(segments);
}

/// The longest daemon name, in cells, the chip shows while it elides the name.
inline constexpr std::size_t LocationChipDaemonElidedMax = 10;

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
            case LocationChipFit::DaemonElided:
                return elideMiddle(daemon, clusters, LocationChipDaemonElidedMax, ElideUnit::Cells);
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
        return joined(clusters.first(kept)) + std::string { ElideMarker };
    }
} // namespace detail

/// @param daemon The daemon's name; empty for an in-process window.
/// @param fit    How much of the chip fits.
/// @return The daemon segment's text: the whole name down to NoStateText, the name middle-elided to at most
///         LocationChipDaemonElidedMax cells at DaemonElided (whole grapheme clusters, the ellipsis counted),
///         nothing from DotOnly on.
[[nodiscard]] inline std::string locationChipDaemonText(std::string_view daemon, LocationChipFit fit)
{
    return detail::daemonTextOf(daemon, clustersOf(daemon), fit);
}

/// Turns a source sentence into what the user reads: Qt's translation in the application, the sentence itself
/// where nothing translates it (the tests).
struct SourceText
{
    /// @param sentence A source-language sentence, with `%1`, `%2` for its arguments.
    /// @return The sentence, as it is.
    [[nodiscard]] std::string operator()(std::string_view sentence) const { return std::string(sentence); }

    /// @param context  The context a shared sentence is translated in.
    /// @param sentence A source-language sentence.
    /// @return The sentence, as it is.
    [[nodiscard]] std::string operator()(std::string_view /*context*/, std::string_view sentence) const
    {
        return std::string(sentence);
    }
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
    return detail::workspaceTextOf(workspace, clustersOf(workspace), fit);
}

/// What a screen reader says when another workspace has news the marker shows: the sentence that starts the
/// description, and the clause that follows another part of it. Two translatable sentences, not one with its
/// first letter changed: whether a clause starts in upper or lower case is the translation's call.
struct LocationChipOtherWorkspaceRow
{
    std::string_view leading; ///< Starts the description; empty where the marker does not show.
    std::string_view
        midList; ///< Follows another part of the description; empty where the marker does not show.
};

/// The rows, indexed by @c Urgency.
inline constexpr std::array<LocationChipOtherWorkspaceRow, 6> LocationChipOtherWorkspaceRows {
    LocationChipOtherWorkspaceRow { .leading = "", .midList = "" }, // Idle
    LocationChipOtherWorkspaceRow { .leading = "", .midList = "" }, // UnseenOutput
    LocationChipOtherWorkspaceRow { .leading = "", .midList = "" }, // Working
    LocationChipOtherWorkspaceRow { .leading = QT_TRANSLATE_NOOP("LocationChip", "Another workspace is done"),
                                    .midList =
                                        QT_TRANSLATE_NOOP("LocationChip", "another workspace is done") },
    LocationChipOtherWorkspaceRow {
        .leading = QT_TRANSLATE_NOOP("LocationChip", "Another workspace has failed"),
        .midList = QT_TRANSLATE_NOOP("LocationChip", "another workspace has failed") },
    LocationChipOtherWorkspaceRow {
        .leading = QT_TRANSLATE_NOOP("LocationChip", "Another workspace needs you"),
        .midList = QT_TRANSLATE_NOOP("LocationChip", "another workspace needs you") },
};

static_assert(std::to_underlying(Urgency::NeedsYou) + 1 == LocationChipOtherWorkspaceRows.size(),
              "LocationChipOtherWorkspaceRows must have a row per Urgency");

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
                                                            Disclosure disclosure,
                                                            Translate const& translate = Translate {})
{
    auto parts = std::vector<std::string> {};
    if (auto const& word = hostChipLook(state).description; !word.empty())
        parts.emplace_back(translate(word));
    if (chipMarkerOf(others) == ChipMarker::Shown)
    {
        auto const& row = LocationChipOtherWorkspaceRows[std::to_underlying(others)];
        parts.emplace_back(translate(parts.empty() ? row.leading : row.midList));
    }
    parts.emplace_back(translate(DisclosureContext, disclosureWord(disclosure)));
    return core::joinHumanReadable(parts);
}

/// @param state What the connection looks like.
/// @return The glyph that stands for the state where a dot cannot be drawn.
[[nodiscard]] constexpr std::string_view daemonStateGlyph(HostChipState state) noexcept
{
    return hostChipLook(state).glyph;
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
    auto const& row = hostChipLook(state);
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
    auto const daemonClusters = clustersOf(daemon);
    auto const workspaceClusters = clustersOf(workspace);
    return std::ranges::to<std::vector<LocationChipStepText>>(
        LocationChipSteps | std::views::transform([&](LocationChipStep const& step) {
            return LocationChipStepText {
                .fit = step.fit,
                .key = step.key,
                .daemon = detail::daemonTextOf(daemon, daemonClusters, step.fit),
                .status = locationChipStatusText(state, step.fit, translate),
                .workspace = detail::workspaceTextOf(workspace, workspaceClusters, step.fit),
                .tooltip = locationChipToolTip(state, step.fit, daemon, translate),
            };
        }));
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
/// @param hosting What serves the window (its row in DaemonHostingPolicies says whether the chip names a
///                daemon).
/// @param sidebar Whether the workspace sidebar shows.
/// @return Where the chip shows: a chip that names a daemon (or a host) follows the band's centre, a window
///         with none (in-process) shows the chip only in the band's workspace-chip centre. The sidebar's
///         head holds the daemon only while the sidebar shows: with it hidden the band holds the chip, so
///         the daemon is named somewhere. With no band, the chip heads the sidebar when that shows.
[[nodiscard]] constexpr LocationChipPlace locationChipPlace(BarPresence band,
                                                            BarCentre centre,
                                                            DaemonHosting hosting,
                                                            SidebarShown sidebar) noexcept
{
    auto const names = daemonHostingPolicy(hosting).naming == ChipNaming::NamesDaemon;
    if (band == BarPresence::Hidden)
        return names && sidebar == SidebarShown::Yes ? LocationChipPlace::Sidebar : LocationChipPlace::None;
    switch (centre)
    {
        case BarCentre::Breadcrumb:
            if (!names)
                return LocationChipPlace::None;
            return sidebar == SidebarShown::Yes ? LocationChipPlace::Sidebar : LocationChipPlace::Band;
        case BarCentre::ChipAndTabs: return LocationChipPlace::Band;
        case BarCentre::Empty:
        case BarCentre::Tabs:
        case BarCentre::Title: return names ? LocationChipPlace::Band : LocationChipPlace::None;
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
