// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/KeyEquivalentMatch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace contour::platform;

namespace
{
constexpr auto DownArrow = char32_t { 0xF701 };
constexpr auto UpArrow = char32_t { 0xF700 };
constexpr auto CtrlCmd = nsmod::Control | nsmod::Command;
// AppKit sets NumericPad and Function on every arrow-key event.
constexpr auto ArrowFlags = nsmod::NumericPad | nsmod::Function;

struct Row
{
    char32_t eventChar;
    uint64_t eventMods;
    char32_t itemChar;
    uint64_t itemMods;
    KeyEquivalentMatch expected;
};
} // namespace

TEST_CASE("KeyEquivalentMatch.classify", "[keyequivalent]")
{
    auto const rows = std::array {
        // plain letters
        Row { U'q', nsmod::Command, U'q', nsmod::Command, KeyEquivalentMatch::Strict },
        Row { U'Q', nsmod::Command | nsmod::Shift, U'q', nsmod::Command, KeyEquivalentMatch::None },
        Row { U'q', nsmod::Command, U'w', nsmod::Command, KeyEquivalentMatch::None },
        Row { U'q', nsmod::Command | nsmod::Option, U'q', nsmod::Command, KeyEquivalentMatch::None },
        // case rules: an uppercase item implies Shift; the event char folds
        Row { U'q', nsmod::Command | nsmod::Shift, U'Q', nsmod::Command, KeyEquivalentMatch::Strict },
        Row { U'Q', nsmod::Command | nsmod::Shift, U'Q', nsmod::Command, KeyEquivalentMatch::Strict },
        Row { U'q', nsmod::Command, U'Q', nsmod::Command, KeyEquivalentMatch::None },
        // arrows: the event carries Function (and NumericPad); the item does not
        Row { DownArrow, CtrlCmd | ArrowFlags, DownArrow, CtrlCmd, KeyEquivalentMatch::IgnoringFunction },
        Row { DownArrow,
              CtrlCmd | ArrowFlags,
              DownArrow,
              CtrlCmd | nsmod::Function,
              KeyEquivalentMatch::Strict },
        Row { UpArrow, CtrlCmd | ArrowFlags, DownArrow, CtrlCmd, KeyEquivalentMatch::None },
        Row { DownArrow, CtrlCmd | ArrowFlags, DownArrow, nsmod::Control, KeyEquivalentMatch::None },
        Row { DownArrow, ArrowFlags, DownArrow, 0, KeyEquivalentMatch::IgnoringFunction },
        // NumericPad alone never decides
        Row { U'1', nsmod::Command | nsmod::NumericPad, U'1', nsmod::Command, KeyEquivalentMatch::Strict },
        // bits outside the compared set (caps lock, device bits) never decide
        Row { U'q', nsmod::Command | 0x10000 | 0x1, U'q', nsmod::Command, KeyEquivalentMatch::Strict },
    };
    for (auto const& row: rows)
        CHECK(classifyKeyEquivalent(row.eventChar, row.eventMods, row.itemChar, row.itemMods)
              == row.expected);
}

TEST_CASE("KeyEquivalentMatch.describeNsModifiers", "[keyequivalent]")
{
    CHECK(describeNsModifiers(0) == "none");
    CHECK(describeNsModifiers(CtrlCmd) == "control|command");
    CHECK(describeNsModifiers(CtrlCmd | ArrowFlags) == "control|command|function|numericPad");
}
