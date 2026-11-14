// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include <vtworkspace/TabName.hpp>

using vtworkspace::TabNameCandidates;
using vtworkspace::TabNameSource;

TEST_CASE("TabName: a label names a tab over its title, either over its panes", "[tab][name]")
{
    struct Row
    {
        std::string_view label;
        std::string_view title;
        TabNameSource source;
        std::string_view name;
    };
    constexpr auto Panes = std::string_view { "zsh" };
    auto const rows = std::array {
        Row { .label = "", .title = "", .source = TabNameSource::Panes, .name = Panes },
        Row { .label = "", .title = "renamed", .source = TabNameSource::Title, .name = "renamed" },
        Row { .label = "build", .title = "", .source = TabNameSource::Label, .name = "build" },
        Row { .label = "build", .title = "renamed", .source = TabNameSource::Label, .name = "build" },
    };
    for (auto const& row: rows)
    {
        INFO("label '" << row.label << "', title '" << row.title << "'");
        auto const candidates = TabNameCandidates { .label = row.label, .title = row.title };
        CHECK(vtworkspace::tabNameSourceOf(candidates) == row.source);
        CHECK(vtworkspace::tabNameOf(candidates, Panes) == row.name);
    }
}

TEST_CASE("TabName: the decision is a constant expression", "[tab][name]")
{
    STATIC_CHECK(vtworkspace::tabNameSourceOf({ .label = "a", .title = "b" }) == TabNameSource::Label);
    STATIC_CHECK(vtworkspace::tabNameOf({}, "pane") == "pane");
}
