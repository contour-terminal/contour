// SPDX-License-Identifier: Apache-2.0
#include <contour/session/ScrollbarVisibility.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <vector>

using contour::config::ScrollBarMode;
using contour::config::ScrollBarPosition;
using contour::session::AltScreenScrollbar;
using contour::session::resolveScrollbarVisibility;
using contour::session::ScrollbarVisibility;
using contour::session::ScrollbarVisibilityInputs;
using vtbackend::MarksAbove;
using vtbackend::ScreenType;

namespace
{

/// Every value of @p Enum, in order, given how many there are (its `…Count` constant).
template <typename Enum, size_t Count>
[[nodiscard]] std::vector<Enum> everyValue()
{
    return std::ranges::to<std::vector>(
        std::views::iota(size_t { 0 }, Count)
        | std::views::transform([](size_t value) { return static_cast<Enum>(value); }));
}

/// One row of the visibility table.
struct VisibilityCase
{
    std::string_view name;
    ScrollbarVisibilityInputs inputs;
    ScrollbarVisibility expected;
};

constexpr auto History = int64_t { 10 };

} // namespace

TEST_CASE("scrollbarVisibility.table", "[scrollbarvisibility]")
{
    using enum ScrollbarVisibility;
    auto const cases = std::array {
        VisibilityCase { .name = "a hidden position wins over everything",
                         .inputs = { .position = ScrollBarPosition::Hidden,
                                     .mode = ScrollBarMode::Always,
                                     .screen = ScreenType::Primary,
                                     .altScreen = AltScreenScrollbar::Shown,
                                     .historyLines = History,
                                     .scrollOffset = 3,
                                     .marksAbove = MarksAbove::Some },
                         .expected = Hidden },
        VisibilityCase { .name = "the alternate screen with hide_in_alt_screen hides",
                         .inputs = { .mode = ScrollBarMode::Always,
                                     .screen = ScreenType::Alternate,
                                     .altScreen = AltScreenScrollbar::Hidden,
                                     .historyLines = History },
                         .expected = Hidden },
        VisibilityCase { .name = "the alternate screen without hide_in_alt_screen follows the rest",
                         .inputs = { .mode = ScrollBarMode::Always,
                                     .screen = ScreenType::Alternate,
                                     .altScreen = AltScreenScrollbar::Shown,
                                     .historyLines = History },
                         .expected = Shown },
        VisibilityCase { .name = "no history hides even an Always bar",
                         .inputs = { .mode = ScrollBarMode::Always, .historyLines = 0 },
                         .expected = Hidden },
        VisibilityCase { .name = "Always at the bottom shows",
                         .inputs = { .mode = ScrollBarMode::Always, .historyLines = History },
                         .expected = Shown },
        VisibilityCase { .name = "Overlay at the bottom hides",
                         .inputs = { .mode = ScrollBarMode::Overlay, .historyLines = History },
                         .expected = Hidden },
        VisibilityCase {
            .name = "Overlay one row up shows",
            .inputs = { .mode = ScrollBarMode::Overlay, .historyLines = History, .scrollOffset = 1 },
            .expected = Shown },
        VisibilityCase { .name = "Overlay at the bottom with a held mark above shows",
                         .inputs = { .mode = ScrollBarMode::Overlay,
                                     .historyLines = History,
                                     .marksAbove = MarksAbove::Some },
                         .expected = Shown },
        VisibilityCase {
            .name = "a held mark keeps nothing on screen without history",
            .inputs = { .mode = ScrollBarMode::Overlay, .historyLines = 0, .marksAbove = MarksAbove::Some },
            .expected = Hidden },
    };
    for (auto const& row: cases)
    {
        INFO(row.name);
        CHECK(resolveScrollbarVisibility(row.inputs) == row.expected);
    }
}

TEST_CASE("scrollbarVisibility.everyCombination", "[scrollbarvisibility]")
{
    // The five rules, written as one expression, independently of the rule table under test.
    auto const expected = [](ScrollbarVisibilityInputs const& in) {
        auto const shown =
            in.position != ScrollBarPosition::Hidden
            && (in.screen != ScreenType::Alternate || in.altScreen != AltScreenScrollbar::Hidden)
            && in.historyLines > 0
            && (in.mode == ScrollBarMode::Always || in.scrollOffset > 0 || in.marksAbove == MarksAbove::Some);
        return shown ? ScrollbarVisibility::Shown : ScrollbarVisibility::Hidden;
    };

    auto const historyCounts = std::array<int64_t, 2> { 0, History };
    auto const scrollOffsets = std::array<int64_t, 2> { 0, 3 };
    auto combinations = size_t { 0 };
    for (auto const position: everyValue<ScrollBarPosition, contour::config::ScrollBarPositionCount>())
        for (auto const mode: everyValue<ScrollBarMode, contour::config::ScrollBarModeCount>())
            for (auto const screen: everyValue<ScreenType, vtbackend::ScreenTypeCount>())
                for (auto const altScreen:
                     everyValue<AltScreenScrollbar, contour::session::AltScreenScrollbarCount>())
                    for (auto const marks: everyValue<MarksAbove, vtbackend::MarksAboveCount>())
                        for (auto const historyLines: historyCounts)
                            for (auto const scrollOffset: scrollOffsets)
                            {
                                auto const inputs = ScrollbarVisibilityInputs { .position = position,
                                                                                .mode = mode,
                                                                                .screen = screen,
                                                                                .altScreen = altScreen,
                                                                                .historyLines = historyLines,
                                                                                .scrollOffset = scrollOffset,
                                                                                .marksAbove = marks };
                                auto const visibility = resolveScrollbarVisibility(inputs);
                                INFO("position "
                                     << static_cast<int>(position) << ", mode " << static_cast<int>(mode)
                                     << ", screen " << static_cast<int>(screen) << ", altScreen "
                                     << static_cast<int>(altScreen) << ", marks " << static_cast<int>(marks)
                                     << ", history " << historyLines << ", offset " << scrollOffset);
                                CHECK(visibility == expected(inputs));
                                if (historyLines == 0)
                                    CHECK(visibility == ScrollbarVisibility::Hidden);
                                ++combinations;
                            }
    // Every enumerator of every input was visited.
    CHECK(combinations
          == everyValue<ScrollBarPosition, contour::config::ScrollBarPositionCount>().size()
                 * everyValue<ScrollBarMode, contour::config::ScrollBarModeCount>().size()
                 * everyValue<ScreenType, vtbackend::ScreenTypeCount>().size()
                 * everyValue<AltScreenScrollbar, contour::session::AltScreenScrollbarCount>().size()
                 * everyValue<MarksAbove, vtbackend::MarksAboveCount>().size() * historyCounts.size()
                 * scrollOffsets.size());
}
