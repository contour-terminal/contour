// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// @brief Column alignment for table cells.
enum class TableAlignment : std::uint8_t
{
    Left,   ///< Left-aligned (default).
    Center, ///< Center-aligned.
    Right,  ///< Right-aligned.
};

/// @brief A fully parsed GFM-style pipe table.
struct ParsedTable
{
    std::vector<std::string> headers;           ///< Header cell texts (trimmed).
    std::vector<TableAlignment> alignments;     ///< Per-column alignment from separator row.
    std::vector<std::vector<std::string>> rows; ///< Data rows, each a vector of cell texts.
    std::size_t columnCount = 0;                ///< Number of columns (from header).
};

/// @brief Checks if a line looks like a table row (starts with `|` after optional whitespace).
/// @param line The line to check.
/// @return true if the line starts with `|`.
[[nodiscard]] auto detectTableRow(std::string_view line) -> bool;

/// @brief Checks if a line is a table separator row (e.g. `|:---|---:|:---:|`).
/// @param line The line to check.
/// @return true if the line is a valid separator row.
[[nodiscard]] auto detectTableSeparator(std::string_view line) -> bool;

/// @brief Splits a pipe-delimited table row into trimmed cell texts.
/// @param line The table row (e.g. `| A | B | C |`).
/// @return Vector of trimmed cell strings.
[[nodiscard]] auto splitTableRow(std::string_view line) -> std::vector<std::string>;

/// @brief Parses alignment markers from a separator row.
/// @param line The separator row (e.g. `|:---|---:|:---:|`).
/// @return Vector of TableAlignment values.
[[nodiscard]] auto parseTableAlignments(std::string_view line) -> std::vector<TableAlignment>;

/// @brief Computes the maximum display width for each column in a parsed table.
/// @param table The parsed table.
/// @return Vector of column widths (minimum 3 per column).
[[nodiscard]] auto computeColumnWidths(ParsedTable const& table) -> std::vector<int>;

/// @brief Pads and aligns a cell text to the given width.
/// @param text The cell text.
/// @param width The target width.
/// @param alignment The alignment to apply.
/// @return The padded cell string.
[[nodiscard]] auto alignCell(std::string_view text, int width, TableAlignment alignment) -> std::string;

/// @brief Constrains column widths to fit within a maximum table width.
///
/// Accounts for border overhead (1 left border + 3 per column for padding + separator).
/// Uses a waterfall algorithm that only shrinks the widest columns, preserving
/// narrow columns at their natural width. Minimum 3 characters per column.
/// @param widths Column widths to constrain (modified in place).
/// @param maxTableWidth Maximum total table width in terminal columns.
void constrainColumnWidths(std::vector<int>& widths, int maxTableWidth);

/// @brief Wraps text at word boundaries to fit within a maximum width.
/// @param text The text to wrap.
/// @param maxWidth The maximum line width.
/// @return A vector of lines, each fitting within maxWidth.
[[nodiscard]] auto wrapText(std::string_view text, int maxWidth) -> std::vector<std::string>;

/// @brief Strips inline markdown formatting markers from text.
///
/// Removes bold (**/**), italic (*), bold (__), inline code (`), and link
/// syntax ([text](url) -> text). Returns the text as it would appear when rendered.
/// @param text The raw markdown text.
/// @return The text with inline formatting markers removed.
[[nodiscard]] auto stripInlineMarkdown(std::string_view text) -> std::string;

/// @brief Computes the display width of text after stripping inline markdown.
/// @param text The raw markdown text.
/// @return The display width as it would appear when rendered.
[[nodiscard]] auto inlineDisplayWidth(std::string_view text) -> int;

/// @brief Truncates text to fit within a maximum display width.
///
/// Uses grapheme cluster segmentation to cut at a display-column boundary.
/// Appends a horizontal ellipsis (U+2026) when truncation occurs.
/// @param text The text to truncate.
/// @param maxWidth The maximum display width in terminal columns.
/// @return The truncated text, or the original if it already fits.
[[nodiscard]] auto truncateToDisplayWidth(std::string_view text, int maxWidth) -> std::string;

} // namespace core::tui
