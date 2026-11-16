// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace core::tui
{

/// @brief Border style for Box rendering.
enum class BorderStyle : std::uint8_t
{
    None,    ///< No border
    Single,  ///< Single line: ─ │ ┌ ┐ └ ┘
    Double,  ///< Double line: ═ ║ ╔ ╗ ╚ ╝
    Rounded, ///< Rounded corners: ─ │ ╭ ╮ ╰ ╯
    Heavy,   ///< Heavy/thick: ━ ┃ ┏ ┓ ┗ ┛
    Dashed,  ///< Dashed: ╌ ╎ ┌ ┐ └ ┘
};

/// @brief Border characters for a box.
struct BorderChars
{
    std::string_view horizontal;  ///< Horizontal line (─)
    std::string_view vertical;    ///< Vertical line (│)
    std::string_view topLeft;     ///< Top-left corner (┌)
    std::string_view topRight;    ///< Top-right corner (┐)
    std::string_view bottomLeft;  ///< Bottom-left corner (└)
    std::string_view bottomRight; ///< Bottom-right corner (┘)
    std::string_view leftT;       ///< Left T-junction (├)
    std::string_view rightT;      ///< Right T-junction (┤)
    std::string_view topT;        ///< Top T-junction (┬)
    std::string_view bottomT;     ///< Bottom T-junction (┴)
    std::string_view cross;       ///< Cross junction (┼)

    /// @brief Returns the border characters for a given style.
    static auto fromStyle(BorderStyle style) noexcept -> BorderChars;
};

/// @brief Title alignment for box titles.
enum class TitleAlign : std::uint8_t
{
    Left,
    Center,
    Right,
};

/// @brief Configuration for rendering a box with borders.
struct BoxConfig
{
    int row = 1;                               ///< Top-left row (1-based)
    int col = 1;                               ///< Top-left column (1-based)
    int width = 0;                             ///< Total width including borders
    int height = 0;                            ///< Total height including borders
    BorderStyle border = BorderStyle::Rounded; ///< Border style
    Style borderStyle;                         ///< Style for the border
    std::optional<std::string> title;          ///< Optional title in top border
    TitleAlign titleAlign = TitleAlign::Left;  ///< Title alignment
    Style titleStyle;                          ///< Style for the title
    int paddingLeft = 1;                       ///< Inner padding from left border
    int paddingRight = 1;                      ///< Inner padding from right border
    int paddingTop = 0;                        ///< Inner padding from top border
    int paddingBottom = 0;                     ///< Inner padding from bottom border
    bool fillBackground = false;               ///< Fill the box interior with spaces
    Style backgroundStyle;                     ///< Style for the background fill
};

/// @brief A bordered box component for TUI layouts.
///
/// Renders a rectangular box with configurable borders, titles, and padding.
/// Can be used standalone or as a container for other content.
class Box
{
  public:
    /// @brief Constructs a box with the given configuration.
    explicit Box(BoxConfig config);

    /// @brief Renders the box frame (borders and optional title).
    /// Does not render any content inside the box.
    /// @param output The terminal output to render to.
    void render(TerminalOutput& output) const;

    /// @brief Returns the inner width (content area width after borders and padding).
    [[nodiscard]] auto innerWidth() const noexcept -> int;

    /// @brief Returns the inner height (content area height after borders and padding).
    [[nodiscard]] auto innerHeight() const noexcept -> int;

    /// @brief Returns the row where content starts (1-based).
    [[nodiscard]] auto contentStartRow() const noexcept -> int;

    /// @brief Returns the column where content starts (1-based).
    [[nodiscard]] auto contentStartCol() const noexcept -> int;

    /// @brief Updates the box configuration.
    void setConfig(BoxConfig config);

    /// @brief Returns the current configuration.
    [[nodiscard]] auto config() const noexcept -> BoxConfig const&;

    /// @brief Clears the content area of the box (fills with spaces).
    /// @param output The terminal output to render to.
    void clearContent(TerminalOutput& output) const;

  private:
    BoxConfig _config;

    void renderTopBorder(TerminalOutput& output, BorderChars const& chars) const;
    void renderBottomBorder(TerminalOutput& output, BorderChars const& chars) const;
    void renderSideBorders(TerminalOutput& output, BorderChars const& chars) const;
};

/// @brief Renders a simple bordered box at the specified position.
/// Convenience function for one-off box rendering.
void renderBox(TerminalOutput& output, BoxConfig const& config);

} // namespace core::tui
