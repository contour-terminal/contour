// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

class Canvas;
class SyntaxHighlighterRegistry;
struct MarkdownTheme;

/// A segment of styled text within a line.
struct StyledSpan
{
    std::string text;
    Style style;
};

/// A line composed of multiple styled spans.
using StyledLine = std::vector<StyledSpan>;

/// @brief A document of styled text that can be rendered to a Canvas.
///
/// StyledText represents pre-parsed and pre-wrapped text content that can be
/// efficiently rendered to a Canvas. It supports both plain text and markdown
/// content, with automatic line wrapping to a specified width.
///
/// This class bridges the gap between text parsing (plain or markdown) and
/// Canvas-based rendering, enabling features like scrollable tooltips.
class StyledText
{
  public:
    StyledText() = default;

    /// @brief Creates StyledText from plain text.
    /// @param text The plain text content.
    /// @param maxWidth Maximum line width (0 = no wrapping).
    /// @param baseStyle Style to apply to all text.
    /// @return The parsed StyledText.
    [[nodiscard]] static StyledText fromPlainText(std::string_view text,
                                                  int maxWidth = 0,
                                                  Style baseStyle = {});

    /// @brief Creates StyledText from markdown.
    /// @param markdown The markdown content.
    /// @param maxWidth Maximum line width (0 = no wrapping).
    /// @param theme Theme for markdown styling (uses defaults if not provided).
    /// @param highlighters The languages a fenced code block may be tagged with beyond the
    ///        built-in ones, or nullptr (the default) for the built-in ones alone.
    /// @return The parsed StyledText.
    [[nodiscard]] static StyledText fromMarkdown(std::string_view markdown,
                                                 int maxWidth = 0,
                                                 MarkdownTheme const* theme = nullptr,
                                                 SyntaxHighlighterRegistry const* highlighters = nullptr);

    /// @brief Returns all lines.
    [[nodiscard]] std::vector<StyledLine> const& lines() const noexcept { return _lines; }

    /// @brief Returns the number of lines.
    [[nodiscard]] int lineCount() const noexcept { return static_cast<int>(_lines.size()); }

    /// @brief Returns the maximum width of any line (for sizing).
    [[nodiscard]] int maxLineWidth() const noexcept { return _maxLineWidth; }

    /// @brief Checks if the content is empty.
    [[nodiscard]] bool empty() const noexcept { return _lines.empty(); }

    /// @brief Renders lines to a canvas.
    /// @param canvas The canvas to render to.
    /// @param startLine First line to render (0-based).
    /// @param lineCount Number of lines to render (-1 = all from startLine).
    void renderTo(Canvas& canvas, int startLine = 0, int lineCount = -1) const;

    /// @brief Clears all content.
    void clear();

  private:
    std::vector<StyledLine> _lines;
    int _maxLineWidth = 0;

    /// @brief Adds a line and updates max width.
    void addLine(StyledLine line);

    /// @brief Calculates display width of a styled line.
    [[nodiscard]] static int lineWidth(StyledLine const& line);

    /// @brief Wraps a line to fit within maxWidth.
    [[nodiscard]] static std::vector<StyledLine> wrapLine(StyledLine const& line, int maxWidth);
};

} // namespace core::tui
