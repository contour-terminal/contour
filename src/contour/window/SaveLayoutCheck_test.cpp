// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SaveLayoutCheck.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

using contour::window::checkLayoutName;
using contour::window::LayoutNameState;
using contour::window::LayoutReplace;
using contour::window::SaveLayoutButton;
using contour::window::SaveLayoutCheck;

TEST_CASE("checkLayoutName: what a typed name offers", "[layout][savecheck]")
{
    auto const existing = std::array { std::string { "dev" }, std::string { "ops" } };
    struct Row
    {
        std::string_view name;
        LayoutReplace replace;
        SaveLayoutCheck expected;
    };
    auto const rows = std::array {
        Row { "", LayoutReplace::Keep, { LayoutNameState::Empty, SaveLayoutButton::Disabled } },
        Row { "web", LayoutReplace::Keep, { LayoutNameState::Available, SaveLayoutButton::Save } },
        Row { "dev", LayoutReplace::Keep, { LayoutNameState::Taken, SaveLayoutButton::Disabled } },
        Row { "dev", LayoutReplace::Replace, { LayoutNameState::Taken, SaveLayoutButton::Replace } },
        Row { "Dev", LayoutReplace::Keep, { LayoutNameState::Available, SaveLayoutButton::Save } },
        Row { "web", LayoutReplace::Replace, { LayoutNameState::Available, SaveLayoutButton::Save } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name << (row.replace == LayoutReplace::Replace ? " (replace)" : ""));
        CHECK(checkLayoutName(row.name, existing, row.replace) == row.expected);
    }
}

TEST_CASE("layoutSummaryText: what Save Layout saves", "[layout][savecheck]")
{
    using contour::window::layoutSummaryText;
    CHECK(layoutSummaryText(1, 1) == "1 tab · 1 pane · working directories and commands");
    CHECK(layoutSummaryText(3, 5) == "3 tabs · 5 panes · working directories and commands");
}
