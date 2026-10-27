// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/BlockPill.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/shell/BlockPill.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

using namespace vtbackend;

namespace
{
/// Metrics whose every measured codepoint advances @p advance pixels, a cell @p cellWidth wide, and each
/// style's pill @p padding wider than its label.
[[nodiscard]] PillLabelMetrics uniformMetrics(int advance, int widest, int cellWidth, int padding)
{
    auto metrics = PillLabelMetrics { .cellWidth = cellWidth };
    for (auto& face: metrics.styles)
    {
        face.advances.fill(advance);
        face.widest = widest;
        face.padding = padding;
    }
    return metrics;
}

/// @p style's name, for a failure message.
[[nodiscard]] std::string nameOf(PillStyle style)
{
    switch (style)
    {
        case PillStyle::Success: return "Success";
        case PillStyle::Failed: return "Failed";
        case PillStyle::Running: return "Running";
        case PillStyle::Fold: return "Fold";
    }
    return "?";
}

/// The schemes whose hovered block takes a lighter tint than the canvas's 5 %, because 5 % would push a text
/// pair that reads at 4.5:1 (or a mark that reads at 3:1) below it. Every other scheme keeps the canvas's
/// tint.
[[nodiscard]] std::set<std::string> lighterHoverTints()
{
    return {
        "contour",       "contour-classic", "contour-dark", "contour-light",    "gruvbox-dark",
        "gruvbox-light", "monokai",         "one-light",    "papercolor-light", "solarized-dark",
    };
}

/// The scheme and style of every pill whose canvas ink is too faint for the scheme, and is lifted.
[[nodiscard]] std::set<std::string> liftedPills()
{
    return {
        "contour Failed",           "contour-classic Failed",  "contour-light Success",
        "gruvbox-dark Failed",      "gruvbox-dark Running",    "gruvbox-light Failed",
        "gruvbox-light Fold",       "gruvbox-light Running",   "gruvbox-light Success",
        "monokai Failed",           "one-dark Failed",         "one-dark Fold",
        "one-dark Success",         "one-light Failed",        "one-light Success",
        "papercolor-dark Failed",   "papercolor-light Fold",   "papercolor-light Running",
        "papercolor-light Success", "solarized-dark Failed",   "solarized-dark Fold",
        "solarized-dark Running",   "solarized-dark Success",  "solarized-light Failed",
        "solarized-light Fold",     "solarized-light Running", "solarized-light Success",
    };
}
} // namespace

TEST_CASE("blockPill.theRowsAreTheCanvas", "[blockpill]")
{
    // The canvas's `blocks` board: Failed status.danger on itself at 16 %, Running the accent alone, a
    // success's duration the label at 62 %, the fold chip the label at 70 % on 8 % with a 12 % ring.
    for (auto const index: std::views::iota(std::size_t { 0 }, PillStyleCount))
        CHECK(static_cast<std::size_t>(PillStyleRows[index].style) == index);

    auto const& failed = pillStyleRow(PillStyle::Failed);
    CHECK(failed.ink == PillInkSource::Status);
    CHECK(failed.outcome == CommandBlockOutcome::Failure);
    CHECK(failed.fillPercent == 16);
    CHECK(failed.ringPercent == 0);

    auto const& running = pillStyleRow(PillStyle::Running);
    CHECK(running.outcome == CommandBlockOutcome::Running);
    CHECK(running.inkPercent == 100);
    CHECK(running.fillPercent == 0);

    auto const& success = pillStyleRow(PillStyle::Success);
    CHECK(success.ink == PillInkSource::Label);
    CHECK(success.inkPercent == 62);
    CHECK(success.fillPercent == 0);

    auto const& fold = pillStyleRow(PillStyle::Fold);
    CHECK(fold.inkPercent == 70);
    CHECK(fold.fillPercent == 8);
    CHECK(fold.ringPercent == 12);
    CHECK(fold.anchor == PillAnchor::Left);
    CHECK(success.anchor == PillAnchor::Right);

    CHECK(pillStyleOf(CommandBlockOutcome::Failure) == PillStyle::Failed);
    CHECK(pillStyleOf(CommandBlockOutcome::Running) == PillStyle::Running);
    CHECK(pillStyleOf(CommandBlockOutcome::Success) == PillStyle::Success);
}

TEST_CASE("blockPill.theMeasuredSetCoversTheLabels", "[blockpill]")
{
    CHECK(pillMeasuredIndex(U' ') == 0u);
    CHECK(pillMeasuredCodepoint(*pillMeasuredIndex(U'~')) == U'~');
    CHECK(pillMeasuredCodepoint(*pillMeasuredIndex(U'·')) == U'·');
    CHECK(pillMeasuredCodepoint(*pillMeasuredIndex(U'⌄')) == U'⌄');
    CHECK_FALSE(pillMeasuredIndex(U'é').has_value());
    for (auto const index: std::views::iota(std::size_t { 0 }, PillMeasuredCount))
        CHECK(pillMeasuredIndex(pillMeasuredCodepoint(index)) == index);
}

TEST_CASE("blockPill.columnsAreTheLabelRoundedUp", "[blockpill]")
{
    CHECK_FALSE(PillLabelMetrics {}.columnsOf(PillStyle::Failed, U"x").has_value());

    // 7-px advances in 10-px cells with 16 px of padding: "4ms" is 21 + 16 = 37 px, four columns.
    auto const metrics = uniformMetrics(7, 12, 10, 16);
    CHECK(metrics.columnsOf(PillStyle::Success, U"4ms") == ColumnCount(4));
    // Exactly 40 px is four columns, not five.
    CHECK(metrics.columnsOf(PillStyle::Success, U"4msx") == ColumnCount(5)); // 44 px
    auto const exact = uniformMetrics(8, 12, 10, 16);
    CHECK(exact.columnsOf(PillStyle::Success, U"abc") == ColumnCount(4)); // 40 px
    // A codepoint the set does not hold counts as the widest.
    CHECK(metrics.columnsOf(PillStyle::Fold, U"é") == ColumnCount(3)); // 12 + 16 = 28 px
    // Never less than a column.
    CHECK(uniformMetrics(1, 1, 10, 0).columnsOf(PillStyle::Fold, U"") == ColumnCount(1));
}

TEST_CASE("blockPill.everyInkReadsAtFourAndAHalf", "[blockpill]")
{
    // Every pill's label reads at 4.5:1 over its pill over the page, on every bundled scheme -- and over a
    // hovered block's tint as well. Where the canvas's value is too faint for a scheme, the ink is lifted,
    // and the schemes that needed it are named here, so a new one is a decision rather than an accident.
    auto lifted = std::set<std::string> {};
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        for (auto const index: std::views::iota(std::size_t { 0 }, PillStyleCount))
        {
            auto const style = static_cast<PillStyle>(index);
            INFO("style: " << nameOf(style));
            auto const inks = pillInks(palette, style);
            CHECK(contrastRatio(inks.ink, inks.ground) >= MinimumTextContrast);
            CHECK(inks.fill.has_value() == (pillStyleRow(style).fillPercent > 0));
            CHECK(inks.ring.has_value() == (pillStyleRow(style).ringPercent > 0));
            if (inks.ink != inks.wanted)
                lifted.insert(std::format("{} {}", name, nameOf(style)));

            auto const tinted = pillInks(palette, style, RGBAColor(palette.defaultForeground, 0x0D));
            CHECK(contrastRatio(tinted.ink, tinted.ground) >= MinimumTextContrast);
        }
    }
    CHECK(lifted == liftedPills());
}

TEST_CASE("blockPill.aHoveredBlocksTintKeepsTheText", "[blockpill][blockhover]")
{
    // The canvas's tint (the label at 5 %) and ring (6 %) of a hovered block. A scheme keeps the 5 % unless
    // it would push a text pair that read at 4.5:1 below it, or a mark that read at 3:1 below that; only
    // those schemes get a lighter tint, and they are named here.
    auto lighter = std::set<std::string> {};
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        auto const inks = blockHoverInks(palette);
        CHECK(inks.ring == RGBAColor(palette.defaultForeground, 0x0F));
        CHECK(inks.tint.rgb() == palette.defaultForeground);
        if (inks.tint.alpha() != 0x0D)
            lighter.insert(std::string(name));

        auto const page = palette.defaultBackground;
        auto const tinted = mixColor(page, inks.tint.rgb(), static_cast<float>(inks.tint.alpha()) / 255.0F);
        auto const check = [&](RGBColor text) {
            auto const before = contrastRatio(text, page);
            auto const after = contrastRatio(text, tinted);
            if (before >= MinimumTextContrast)
                CHECK(after >= MinimumTextContrast);
            else if (before >= MinimumMarkContrast)
                CHECK(after >= MinimumMarkContrast);
        };
        check(palette.defaultForeground);
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 16 }))
        {
            CAPTURE(index);
            check(palette.palette.at(index));
        }
    }
    CHECK(lighter == lighterHoverTints());
}
