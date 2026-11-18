// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the chrome marks a tab a daemon-scoped extension draws: the marker on its tab, and what the strip in
/// its pane's header names. Pure and Qt-free: the words are put together, translated, by
/// ServedChromeText.hpp.
///
/// The mark comes from the daemon (its LayoutState), never from the tab's title: a program that titles its
/// own tab "◈ Agents" gets no marker and no strip, so a served tab cannot be faked from inside a terminal.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace contour::window
{

/// Who draws a tab, as the daemon says: the window-side copy of vthost::proto::WireServedMark.
struct ServedMark
{
    std::string extension;    ///< The extension's name.
    std::string title;        ///< Its display title (its manifest's `title`); empty for none.
    std::string moduleDigest; ///< The hex SHA-256 of the module that draws the tab; empty when unknown.

    bool operator==(ServedMark const&) const = default;
};

/// What a tab carries in its badge row beyond its badge.
enum class TabMarker : uint8_t
{
    None = 0, ///< A tab whose panes run programs.
    Served,   ///< A tab a daemon-scoped extension draws.
};

/// How the chrome marks one tab: the marker, and the parts its pane's strip names.
struct ServedChrome
{
    TabMarker marker = TabMarker::None; ///< The tab's marker.
    std::string title {};               ///< What the tab is named by: the title, else the extension's name.
    std::string extension {};           ///< The extension's name.
    std::string digest {}; ///< The module digest's first ServedStripDigestDigits; empty: unknown.

    bool operator==(ServedChrome const&) const = default;
};

/// How many hex digits of the module digest the strip shows: enough to tell two builds apart at a glance.
inline constexpr std::size_t ServedStripDigestDigits = 12;

/// Decides how the chrome marks a tab.
/// @param served Who draws the tab, as the daemon says; nullopt for a tab whose panes run programs.
/// @return Served and the strip's parts -- the title (the extension's name for an empty one), the extension,
///         the digest's first 12 hex digits -- for a served tab; None and no parts otherwise. The tab's
///         title never decides it.
[[nodiscard]] inline ServedChrome servedChromeOf(std::optional<ServedMark> const& served)
{
    if (!served)
        return {};
    return ServedChrome {
        .marker = TabMarker::Served,
        .title = served->title.empty() ? served->extension : served->title,
        .extension = served->extension,
        .digest =
            std::string { std::string_view { served->moduleDigest }.substr(0, ServedStripDigestDigits) },
    };
}

} // namespace contour::window
