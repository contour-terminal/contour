// SPDX-License-Identifier: Apache-2.0
#include <contour/config/OptionAsAlt.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

using contour::config::configEnumValues;
using contour::config::OptionAsAlt;
using contour::config::OptionKeyRole;
using contour::config::optionKeyRole;
using contour::config::OptionKeySide;
using contour::config::parseOptionAsAlt;

namespace
{

/// Every setting, derived from the table rather than listed.
constexpr auto Settings =
    std::array { OptionAsAlt::Off, OptionAsAlt::Left, OptionAsAlt::Right, OptionAsAlt::Both };

/// Every combination of Option keys that can be down.
constexpr auto Sides =
    std::array { OptionKeySide::None, OptionKeySide::Left, OptionKeySide::Right, OptionKeySide::Both };

} // namespace

TEST_CASE("optionAsAlt.theTableNamesEverySetting", "[optionasalt]")
{
    auto const& table = configEnumValues<OptionAsAlt>();
    REQUIRE(table.size() == Settings.size());
    for (auto const index: std::views::iota(size_t { 0 }, table.size()))
        CHECK(table[index].value == Settings[index]);
    CHECK(table[0].token == "off");
    CHECK(table[1].token == "left");
    CHECK(table[2].token == "right");
    CHECK(table[3].token == "both");
}

TEST_CASE("optionAsAlt.parses", "[optionasalt]")
{
    struct Row
    {
        std::string_view text;
        std::optional<OptionAsAlt> expected;
    };
    auto const rows = std::array {
        Row { "off", OptionAsAlt::Off },     Row { "Left", OptionAsAlt::Left },
        Row { "RIGHT", OptionAsAlt::Right }, Row { "both", OptionAsAlt::Both },
        Row { "true", OptionAsAlt::Both },   Row { "yes", OptionAsAlt::Both },
        Row { "on", OptionAsAlt::Both },     Row { "y", OptionAsAlt::Both },
        Row { "false", OptionAsAlt::Off },   Row { "no", OptionAsAlt::Off },
        Row { "n", OptionAsAlt::Off },       Row { "True", OptionAsAlt::Both },
        Row { "maybe", std::nullopt },       Row { "", std::nullopt },
    };
    for (auto const& row: rows)
    {
        INFO("text '" << row.text << "'");
        CHECK(parseOptionAsAlt(row.text) == row.expected);
    }
}

TEST_CASE("optionAsAlt.aHeldKeyIsAltWhereTheSettingNamesIt", "[optionasalt]")
{
    // One row per (setting, keys down), counted from the enums: sixteen.
    struct Row
    {
        OptionAsAlt setting;
        OptionKeySide down;
        OptionKeyRole expected;
    };
    constexpr auto Alt = OptionKeyRole::Alt;
    constexpr auto Composes = OptionKeyRole::Composes;
    auto const rows = std::array {
        Row { OptionAsAlt::Off, OptionKeySide::None, Composes },
        Row { OptionAsAlt::Off, OptionKeySide::Left, Composes },
        Row { OptionAsAlt::Off, OptionKeySide::Right, Composes },
        Row { OptionAsAlt::Off, OptionKeySide::Both, Composes },
        Row { OptionAsAlt::Left, OptionKeySide::None, Composes },
        Row { OptionAsAlt::Left, OptionKeySide::Left, Alt },
        Row { OptionAsAlt::Left, OptionKeySide::Right, Composes },
        Row { OptionAsAlt::Left, OptionKeySide::Both, Alt },
        Row { OptionAsAlt::Right, OptionKeySide::None, Composes },
        Row { OptionAsAlt::Right, OptionKeySide::Left, Composes },
        Row { OptionAsAlt::Right, OptionKeySide::Right, Alt },
        Row { OptionAsAlt::Right, OptionKeySide::Both, Alt },
        Row { OptionAsAlt::Both, OptionKeySide::None, Composes },
        Row { OptionAsAlt::Both, OptionKeySide::Left, Alt },
        Row { OptionAsAlt::Both, OptionKeySide::Right, Alt },
        Row { OptionAsAlt::Both, OptionKeySide::Both, Alt },
    };
    CHECK(rows.size() == Settings.size() * Sides.size());
    for (auto const& row: rows)
    {
        INFO("setting " << static_cast<int>(row.setting) << ", down " << static_cast<int>(row.down));
        CHECK(optionKeyRole(row.setting, row.down) == row.expected);
    }
    // The table covers every pair once.
    for (auto const setting: Settings)
        for (auto const down: Sides)
            CHECK(std::ranges::count_if(
                      rows, [&](Row const& row) { return row.setting == setting && row.down == down; })
                  == 1);
}

TEST_CASE("option_as_alt: both is Alt whichever side bits the held chord carries", "[option_as_alt]")
{
    using contour::config::optionKeyRoleForHeldChord;
    for (auto const down:
         { OptionKeySide::None, OptionKeySide::Left, OptionKeySide::Right, OptionKeySide::Both })
        CHECK(optionKeyRoleForHeldChord(OptionAsAlt::Both, down) == OptionKeyRole::Alt);
    // The other settings still name their sides.
    CHECK(optionKeyRoleForHeldChord(OptionAsAlt::Left, OptionKeySide::None) == OptionKeyRole::Composes);
    CHECK(optionKeyRoleForHeldChord(OptionAsAlt::Left, OptionKeySide::Left) == OptionKeyRole::Alt);
    CHECK(optionKeyRoleForHeldChord(OptionAsAlt::Off, OptionKeySide::Both) == OptionKeyRole::Composes);
}
