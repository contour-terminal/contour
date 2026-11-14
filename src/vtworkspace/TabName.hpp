// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace vtworkspace
{

/// What names a tab. Every place that names one (the tab strip, the sidebar, the switcher, the status line)
/// asks tabNameSourceOf() which of these wins, so they cannot come to disagree.
enum class TabNameSource : uint8_t
{
    Panes, ///< Its panes, as the place naming the tab names a tab after them (a title, a template).
    Title, ///< Its own title: the rename the user gave it.
    Label, ///< Its control-API label (`contour label tab:… NAME`).
};

/// What a tab could be named by besides its panes; an empty one names nothing.
struct TabNameCandidates
{
    std::string_view label {}; ///< Its control-API label.
    std::string_view title {}; ///< Its own title, a rename.
};

/// One source that names a tab over its panes, and where its text is.
struct TabNameRule
{
    TabNameSource source;                       ///< The source.
    std::string_view TabNameCandidates::* text; ///< Its text among the candidates.
};

/// The sources that name a tab over its panes, in precedence order: the first that names something wins, and
/// where none does, its panes name it. A new source is a row here.
constexpr auto TabNamePrecedence = std::array {
    TabNameRule { .source = TabNameSource::Label, .text = &TabNameCandidates::label },
    TabNameRule { .source = TabNameSource::Title, .text = &TabNameCandidates::title },
};

/// @param candidates What the tab could be named by besides its panes.
/// @return The source that names the tab: a label over a title, either over its panes.
[[nodiscard]] constexpr TabNameSource tabNameSourceOf(TabNameCandidates const& candidates) noexcept
{
    for (auto const& rule: TabNamePrecedence)
        if (!(candidates.*rule.text).empty())
            return rule.source;
    return TabNameSource::Panes;
}

/// @param candidates What the tab could be named by besides its panes.
/// @param panes What names the tab after its panes, where nothing else does.
/// @return The tab's name, from the source tabNameSourceOf() picks.
[[nodiscard]] constexpr std::string_view tabNameOf(TabNameCandidates const& candidates,
                                                   std::string_view panes) noexcept
{
    for (auto const& rule: TabNamePrecedence)
        if (!(candidates.*rule.text).empty())
            return candidates.*rule.text;
    return panes;
}

} // namespace vtworkspace
