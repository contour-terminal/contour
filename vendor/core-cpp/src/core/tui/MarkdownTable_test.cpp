// SPDX-License-Identifier: Apache-2.0
#include <core/tui/MarkdownTable.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;

// ============================================================================
// detectTableRow tests
// ============================================================================

TEST_CASE("MarkdownTable.detectTableRow.basic")
{
    CHECK(detectTableRow("| A | B |"));
    CHECK(detectTableRow("| A |"));
    CHECK(detectTableRow("|A|B|"));
    CHECK(detectTableRow("  | A | B |")); // leading whitespace
}

TEST_CASE("MarkdownTable.detectTableRow.not_a_row")
{
    CHECK_FALSE(detectTableRow(""));
    CHECK_FALSE(detectTableRow("Hello world"));
    CHECK_FALSE(detectTableRow("- list item"));
    CHECK_FALSE(detectTableRow("# heading"));
}

// ============================================================================
// detectTableSeparator tests
// ============================================================================

TEST_CASE("MarkdownTable.detectTableSeparator.basic")
{
    CHECK(detectTableSeparator("|---|---|"));
    CHECK(detectTableSeparator("| --- | --- |"));
    CHECK(detectTableSeparator("|:---|---:|"));
    CHECK(detectTableSeparator("|:---:|:---:|"));
    CHECK(detectTableSeparator("| :--- | ---: | :---: |"));
    CHECK(detectTableSeparator("|---------|"));
}

TEST_CASE("MarkdownTable.detectTableSeparator.not_a_separator")
{
    CHECK_FALSE(detectTableSeparator("| A | B |"));
    CHECK_FALSE(detectTableSeparator("|   |   |"));
    CHECK_FALSE(detectTableSeparator(""));
    CHECK_FALSE(detectTableSeparator("Hello"));
    CHECK_FALSE(detectTableSeparator("| :abc: |"));
}

// ============================================================================
// splitTableRow tests
// ============================================================================

TEST_CASE("MarkdownTable.splitTableRow.basic")
{
    auto const cells = splitTableRow("| Alice | 30 | Berlin |");
    REQUIRE(cells.size() == 3);
    CHECK(cells[0] == "Alice");
    CHECK(cells[1] == "30");
    CHECK(cells[2] == "Berlin");
}

TEST_CASE("MarkdownTable.splitTableRow.no_trailing_pipe")
{
    auto const cells = splitTableRow("| A | B ");
    REQUIRE(cells.size() == 2);
    CHECK(cells[0] == "A");
    CHECK(cells[1] == "B");
}

TEST_CASE("MarkdownTable.splitTableRow.empty_cells")
{
    auto const cells = splitTableRow("| | B | |");
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].empty());
    CHECK(cells[1] == "B");
    CHECK(cells[2].empty());
}

TEST_CASE("MarkdownTable.splitTableRow.single_cell")
{
    auto const cells = splitTableRow("| Hello |");
    REQUIRE(cells.size() == 1);
    CHECK(cells[0] == "Hello");
}

// ============================================================================
// parseTableAlignments tests
// ============================================================================

TEST_CASE("MarkdownTable.parseTableAlignments.all_types")
{
    auto const aligns = parseTableAlignments("| :--- | ---: | :---: | --- |");
    REQUIRE(aligns.size() == 4);
    CHECK(aligns[0] == TableAlignment::Left);
    CHECK(aligns[1] == TableAlignment::Right);
    CHECK(aligns[2] == TableAlignment::Center);
    CHECK(aligns[3] == TableAlignment::Left);
}

TEST_CASE("MarkdownTable.parseTableAlignments.default_left")
{
    auto const aligns = parseTableAlignments("|---|---|");
    REQUIRE(aligns.size() == 2);
    CHECK(aligns[0] == TableAlignment::Left);
    CHECK(aligns[1] == TableAlignment::Left);
}

// ============================================================================
// computeColumnWidths tests
// ============================================================================

TEST_CASE("MarkdownTable.computeColumnWidths.basic")
{
    auto table = ParsedTable {};
    table.headers = { "Name", "Age", "City" };
    table.columnCount = 3;
    table.rows = { { "Alice", "30", "Berlin" }, { "Bob", "25", "London" } };
    table.alignments = { TableAlignment::Left, TableAlignment::Right, TableAlignment::Center };

    auto const widths = computeColumnWidths(table);
    REQUIRE(widths.size() == 3);
    CHECK(widths[0] == 5); // "Alice" is widest
    CHECK(widths[1] == 3); // "Age" == "30" both width 3
    CHECK(widths[2] == 6); // "Berlin"/"London" are widest
}

TEST_CASE("MarkdownTable.computeColumnWidths.minimum_3")
{
    auto table = ParsedTable {};
    table.headers = { "A", "B" };
    table.columnCount = 2;
    table.rows = { { "x", "y" } };
    table.alignments = { TableAlignment::Left, TableAlignment::Left };

    auto const widths = computeColumnWidths(table);
    CHECK(widths[0] == 3); // minimum
    CHECK(widths[1] == 3); // minimum
}

// ============================================================================
// alignCell tests
// ============================================================================

TEST_CASE("MarkdownTable.alignCell.left")
{
    CHECK(alignCell("Hi", 6, TableAlignment::Left) == "Hi    ");
}

TEST_CASE("MarkdownTable.alignCell.right")
{
    CHECK(alignCell("Hi", 6, TableAlignment::Right) == "    Hi");
}

TEST_CASE("MarkdownTable.alignCell.center")
{
    CHECK(alignCell("Hi", 6, TableAlignment::Center) == "  Hi  ");
}

TEST_CASE("MarkdownTable.alignCell.center_odd_padding")
{
    CHECK(alignCell("Hi", 7, TableAlignment::Center) == "  Hi   ");
}

TEST_CASE("MarkdownTable.alignCell.exact_width")
{
    CHECK(alignCell("Hello", 5, TableAlignment::Left) == "Hello");
    CHECK(alignCell("Hello", 5, TableAlignment::Right) == "Hello");
    CHECK(alignCell("Hello", 5, TableAlignment::Center) == "Hello");
}

// ============================================================================
// constrainColumnWidths tests
// ============================================================================

TEST_CASE("MarkdownTable.constrainColumnWidths.fits")
{
    // Table with 2 columns of width 10 each.
    // Overhead = 1 + 2*3 = 7. Total = 7 + 20 = 27. maxWidth = 80 → no change.
    auto widths = std::vector<int> { 10, 10 };
    constrainColumnWidths(widths, 80);
    CHECK(widths[0] == 10);
    CHECK(widths[1] == 10);
}

TEST_CASE("MarkdownTable.constrainColumnWidths.equal_columns_shrink_evenly")
{
    // Table with 2 columns of width 40 each.
    // Overhead = 1 + 2*3 = 7. Total content = 80. maxWidth = 47 → availableContent = 40.
    // Both columns equal width, so both shrink equally to 20.
    auto widths = std::vector<int> { 40, 40 };
    constrainColumnWidths(widths, 47);
    CHECK(widths[0] + widths[1] <= 40);
    CHECK(widths[0] == 20);
    CHECK(widths[1] == 20);
}

TEST_CASE("MarkdownTable.constrainColumnWidths.respects_minimum")
{
    // Very narrow terminal — columns should not go below minimum 3.
    auto widths = std::vector<int> { 50, 50 };
    constrainColumnWidths(widths, 15);
    CHECK(widths[0] >= 3);
    CHECK(widths[1] >= 3);
}

TEST_CASE("MarkdownTable.constrainColumnWidths.waterfall_preserves_narrow")
{
    // Narrow column (14) + wide column (322), maxWidth = 156.
    // Overhead = 1 + 2*3 = 7. availableContent = 149.
    // The narrow column should be preserved at 14, wide column absorbs all.
    auto widths = std::vector<int> { 14, 322 };
    constrainColumnWidths(widths, 156);
    CHECK(widths[0] == 14);
    CHECK(widths[1] == 135); // 149 - 14 = 135
}

TEST_CASE("MarkdownTable.constrainColumnWidths.waterfall_both_shrink_when_needed")
{
    // Both columns exceed fair share — both must shrink.
    // Two columns of 30 and 50, maxWidth = 27 → overhead = 7, available = 20.
    // Excess = 60. Tier 0: width 50 (1 col), next tier = 30. Can reduce 50→30 = 20 removed.
    // Remaining excess = 60 - 20 = 40. Now both at 30. Tier 1: both at 30, reduce toward min 3.
    // Each absorbs 20: both become 10. Total = 20. ✓
    auto widths = std::vector<int> { 30, 50 };
    constrainColumnWidths(widths, 27);
    CHECK(widths[0] + widths[1] <= 20);
    CHECK(widths[0] >= 3);
    CHECK(widths[1] >= 3);
}

TEST_CASE("MarkdownTable.constrainColumnWidths.waterfall_single_column")
{
    // Single column of width 50, maxWidth = 20 → overhead = 4, available = 16.
    auto widths = std::vector<int> { 50 };
    constrainColumnWidths(widths, 20);
    CHECK(widths[0] == 16);
}

TEST_CASE("MarkdownTable.constrainColumnWidths.waterfall_three_columns")
{
    // Three columns: [5, 10, 100], maxWidth = 30 → overhead = 10, available = 20.
    // Excess = 95. Tier 0: col[2]=100, next tier=10. Max reduction = 90.
    // 90 >= 95? No. So reduce col[2] to 10. Excess now = 95 - 90 = 5.
    // Tier 1: col[1]=10, col[2]=10 (2 cols). next tier=5. Max reduction per col = 5, total = 10.
    // 10 >= 5? Yes. Each loses 2 or 3 (5/2=2 r1).
    auto widths = std::vector<int> { 5, 10, 100 };
    constrainColumnWidths(widths, 30);
    CHECK(widths[0] == 5); // preserved
    CHECK(widths[0] + widths[1] + widths[2] == 20);
    CHECK(widths[1] >= 3);
    CHECK(widths[2] >= 3);
}

// ============================================================================
// wrapText tests
// ============================================================================

TEST_CASE("MarkdownTable.wrapText.short_text")
{
    auto const lines = wrapText("Hello world", 20);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "Hello world");
}

TEST_CASE("MarkdownTable.wrapText.wraps_at_word_boundary")
{
    auto const lines = wrapText("Hello beautiful world", 10);
    REQUIRE(lines.size() >= 2);
    // First line should contain "Hello"
    CHECK(lines[0] == "Hello");
    // Second line should contain "beautiful"
    CHECK(lines[1] == "beautiful");
    // Third line should contain "world"
    REQUIRE(lines.size() == 3);
    CHECK(lines[2] == "world");
}

TEST_CASE("MarkdownTable.wrapText.long_word_broken_at_char_boundary")
{
    // A single word longer than maxWidth is broken at character boundaries
    auto const lines = wrapText("superlongword", 5);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "super");
    CHECK(lines[1] == "longw");
    CHECK(lines[2] == "ord");
}

TEST_CASE("MarkdownTable.wrapText.empty_text")
{
    auto const lines = wrapText("", 10);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].empty());
}

// ============================================================================
// stripInlineMarkdown tests
// ============================================================================

TEST_CASE("MarkdownTable.stripInlineMarkdown.plain_text")
{
    CHECK(stripInlineMarkdown("Hello world") == "Hello world");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.bold_asterisks")
{
    CHECK(stripInlineMarkdown("**bold**") == "bold");
    CHECK(stripInlineMarkdown("a **bold** b") == "a bold b");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.bold_underscores")
{
    CHECK(stripInlineMarkdown("__bold__") == "bold");
    CHECK(stripInlineMarkdown("a __bold__ b") == "a bold b");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.italic")
{
    CHECK(stripInlineMarkdown("*italic*") == "italic");
    CHECK(stripInlineMarkdown("a *italic* b") == "a italic b");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.inline_code")
{
    CHECK(stripInlineMarkdown("`code`") == "code");
    CHECK(stripInlineMarkdown("a `code` b") == "a code b");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.link")
{
    CHECK(stripInlineMarkdown("[text](http://url)") == "text");
    CHECK(stripInlineMarkdown("a [link](url) b") == "a link b");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.mixed")
{
    CHECK(stripInlineMarkdown("**bold** and *italic* and `code`") == "bold and italic and code");
}

// ============================================================================
// inlineDisplayWidth tests
// ============================================================================

TEST_CASE("MarkdownTable.inlineDisplayWidth.plain_text")
{
    CHECK(inlineDisplayWidth("Hello") == 5);
}

TEST_CASE("MarkdownTable.inlineDisplayWidth.bold_text")
{
    CHECK(inlineDisplayWidth("**bold**") == 4);
    CHECK(inlineDisplayWidth("**Functional Core**") == 15);
}

TEST_CASE("MarkdownTable.inlineDisplayWidth.inline_code")
{
    CHECK(inlineDisplayWidth("`code`") == 4);
}

TEST_CASE("MarkdownTable.inlineDisplayWidth.double_backtick_inline_code")
{
    // ``code with ` backtick`` — content is "code with ` backtick" (20 chars)
    CHECK(inlineDisplayWidth("``code with ` backtick``") == 20);
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.double_backtick")
{
    CHECK(stripInlineMarkdown("``code with ` backtick``") == "code with ` backtick");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.double_backtick_space_stripping")
{
    // CommonMark: `` `code` `` → content " `code` " → strip spaces → "`code`"
    CHECK(stripInlineMarkdown("`` `code` ``") == "`code`");
}

// ============================================================================
// truncateToDisplayWidth tests
// ============================================================================

TEST_CASE("MarkdownTable.truncateToDisplayWidth.no_truncation_needed")
{
    CHECK(truncateToDisplayWidth("Hello", 10) == "Hello");
    CHECK(truncateToDisplayWidth("Hi", 2) == "Hi");
}

TEST_CASE("MarkdownTable.truncateToDisplayWidth.truncates_with_ellipsis")
{
    auto const result = truncateToDisplayWidth("Hello world", 6);
    // Should be at most 6 display columns, ending with ellipsis
    CHECK(result.contains("\xe2\x80\xa6")); // contains …
    CHECK(inlineDisplayWidth(result) <= 6);
}

TEST_CASE("MarkdownTable.truncateToDisplayWidth.empty_on_zero_width")
{
    CHECK(truncateToDisplayWidth("Hello", 0).empty());
}

// ============================================================================
// computeColumnWidths with inline markdown
// ============================================================================

TEST_CASE("MarkdownTable.computeColumnWidths.strips_markdown")
{
    auto table = ParsedTable {};
    table.headers = { "**Header**", "Plain" };
    table.columnCount = 2;
    table.rows = { { "text", "data" } };
    table.alignments = { TableAlignment::Left, TableAlignment::Left };

    auto const widths = computeColumnWidths(table);
    CHECK(widths[0] == 6); // "Header" = 6 (not "**Header**" = 10)
    CHECK(widths[1] == 5); // "Plain" = 5
}

// ============================================================================
// stripInlineMarkdown must match what MarkdownRenderer::renderInline() draws,
// or column widths are computed from text that is never displayed.
// ============================================================================

TEST_CASE("MarkdownTable.stripInlineMarkdown.badge_reduces_to_alt_text")
{
    CHECK(stripInlineMarkdown("[![Linux](badge.svg)](https://ci)") == "Linux");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.image_reduces_to_alt_text")
{
    CHECK(stripInlineMarkdown("![alt](x.png)") == "alt");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.autolink_drops_angle_brackets")
{
    CHECK(stripInlineMarkdown("<https://x.com>") == "https://x.com");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.html_anchor_reduces_to_label")
{
    CHECK(stripInlineMarkdown(R"(<a href="https://y.com">Y</a>)") == "Y");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.html_img_reduces_to_alt")
{
    CHECK(stripInlineMarkdown(R"(<img src="x.png" alt="pic">)") == "pic");
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.lone_angle_bracket_preserved")
{
    CHECK(stripInlineMarkdown("a < b") == "a < b");
}

TEST_CASE("MarkdownTable.inlineDisplayWidth.matches_rendered_width_for_links")
{
    // Regression: the autolink used to measure 15 (with brackets) but render 13.
    CHECK(inlineDisplayWidth("<https://x.com>") == 13);
    CHECK(inlineDisplayWidth(R"(<a href="https://y.com">Y</a>)") == 1);
    CHECK(inlineDisplayWidth("[![Linux](badge.svg)](https://ci)") == 5);
}

TEST_CASE("MarkdownTable.stripInlineMarkdown.link_title_is_not_displayed")
{
    CHECK(stripInlineMarkdown(R"([Docs](https://x.com "Title"))") == "Docs");
    CHECK(stripInlineMarkdown(R"(![alt](x.png "Title"))") == "alt");
}
