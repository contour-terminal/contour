// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// @brief Text alignment options.
enum class TextAlign : std::uint8_t
{
    Left,
    Center,
    Right,
};

/// @brief Wrapping mode for text.
enum class WrapMode : std::uint8_t
{
    None, ///< No wrapping, truncate with ellipsis.
    Char, ///< Wrap at character boundaries.
    Word, ///< Wrap at word boundaries (preferred).
};

/// @brief A styled text span within a line.
struct TextSpan
{
    std::string text; ///< The text content.
    Style style;      ///< The style for this span.
};

/// @brief A line of styled text composed of multiple spans.
struct TextLine
{
    std::vector<TextSpan> spans; ///< The spans in this line.

    /// @brief Returns the total display width of the line.
    [[nodiscard]] auto width() const noexcept -> int;

    /// @brief Appends a span to the line.
    void append(std::string text, Style const& style = {});

    /// @brief Appends plain text to the line.
    void append(std::string_view text);
};

/// @brief A text component with support for styling, wrapping, and alignment.
///
/// Can render single or multi-line text with word wrapping and various
/// alignment options. Supports styled text through TextLine/TextSpan.
class Text
{
  public:
    /// @brief Constructs an empty text component.
    Text() = default;

    /// @brief Constructs a text component with the given content.
    /// @param content The text content.
    /// @param style The default style.
    explicit Text(std::string_view content, Style const& style = {});

    /// @brief Sets the text content (single style).
    /// @param content The text content.
    /// @param style The style to apply.
    void setText(std::string_view content, Style const& style = {});

    /// @brief Sets the text content from styled lines.
    /// @param lines The styled text lines.
    void setLines(std::vector<TextLine> lines);

    /// @brief Returns the text content as a plain string.
    [[nodiscard]] auto text() const -> std::string;

    /// @brief Returns the styled lines.
    [[nodiscard]] auto lines() const noexcept -> std::vector<TextLine> const&;

    /// @brief Sets the alignment.
    /// @param align The text alignment.
    void setAlign(TextAlign align);

    /// @brief Returns the current alignment.
    [[nodiscard]] auto align() const noexcept -> TextAlign;

    /// @brief Sets the wrapping mode.
    /// @param mode The wrapping mode.
    void setWrapMode(WrapMode mode);

    /// @brief Returns the current wrapping mode.
    [[nodiscard]] auto wrapMode() const noexcept -> WrapMode;

    /// @brief Sets the maximum width for wrapping.
    /// @param width The maximum width (0 = no limit).
    void setMaxWidth(int width);

    /// @brief Returns the maximum width.
    [[nodiscard]] auto maxWidth() const noexcept -> int;

    /// @brief Renders the text at the specified position.
    /// @param output The terminal output to render to.
    /// @param startRow The starting row (1-based).
    /// @param startCol The starting column (1-based).
    /// @param width The available width for rendering.
    void render(TerminalOutput& output, int startRow, int startCol, int width) const;

    /// @brief Returns the number of lines that would be rendered at the given width.
    /// @param width The available width.
    [[nodiscard]] auto lineCount(int width) const -> int;

    /// @brief Calculates the wrapped lines for the given width.
    /// @param width The available width.
    [[nodiscard]] auto wrap(int width) const -> std::vector<TextLine>;

  private:
    std::vector<TextLine> _lines;
    TextAlign _align = TextAlign::Left;
    WrapMode _wrapMode = WrapMode::Word;
    int _maxWidth = 0;
};

/// @brief Word-wraps a string at the given width.
/// @param text The text to wrap.
/// @param width The maximum line width.
/// @return A vector of wrapped lines.
[[nodiscard]] auto wordWrap(std::string_view text, int width) -> std::vector<std::string>;

/// @brief Truncates a string to fit within the given width, adding ellipsis if needed.
/// @param text The text to truncate.
/// @param width The maximum width.
/// @return The truncated string.
[[nodiscard]] auto truncate(std::string_view text, int width) -> std::string;

/// @brief Returns the display width of a string (accounting for multi-byte characters).
/// @param text The text to measure.
[[nodiscard]] auto displayWidth(std::string_view text) -> int;

} // namespace core::tui
