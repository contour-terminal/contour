// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsSearch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using contour::window::matchSettings;
using contour::window::SettingsGroup;
using contour::window::SettingsSearchEntry;

namespace
{
/// A handful of rows, as the page lists them.
[[nodiscard]] std::vector<SettingsSearchEntry> entries()
{
    return {
        { .key = "reflow_on_resize",
          .label = "Reflow on resize",
          .help = "Rewrap lines when the window resizes; scrolling keeps its place.",
          .group = SettingsGroup::GeneralWindows },
        { .key = "smooth_line_scrolling",
          .label = "Smooth scrolling time",
          .help = "How long scrolling by a line animates.",
          .group = SettingsGroup::ProfileScrolling },
        { .key = "history_max_lines",
          .label = "Scrollback lines",
          .help = "How many lines the history keeps.",
          .group = SettingsGroup::ProfileHistory },
        { .key = "gutter.timestamps",
          .label = "Show timestamps",
          .help = "When each command ran.",
          .group = SettingsGroup::ShellGutter },
    };
}
} // namespace

TEST_CASE("SettingsSearch: a label match comes before a help match, then a key match", "[settingssearch]")
{
    auto const all = entries();
    // "scroll" is in the help of row 0, listed first, and in the labels of rows 1 and 2.
    CHECK(matchSettings("scroll", all) == std::vector<std::size_t> { 1, 2, 0 });
    CHECK(matchSettings("Scrollback", all) == std::vector<std::size_t> { 2 });
}

TEST_CASE("SettingsSearch: a key matches, in any case", "[settingssearch]")
{
    auto const all = entries();
    CHECK(matchSettings("GUTTER.", all) == std::vector<std::size_t> { 3 });
    CHECK(matchSettings("reflow_on", all) == std::vector<std::size_t> { 0 });
}

TEST_CASE("SettingsSearch: an empty or blank query matches nothing", "[settingssearch]")
{
    auto const all = entries();
    CHECK(matchSettings("", all).empty());
    CHECK(matchSettings("  ", all).empty());
    CHECK(matchSettings("zzz", all).empty());
}
