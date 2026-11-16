// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Box.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/StyledText.hpp>

#include <string>
#include <string_view>

namespace core::tui
{

/// @brief Content type for tooltip text.
enum class TooltipContentType : std::uint8_t
{
    PlainText, ///< Simple text, auto-wrapped
    Markdown,  ///< Parsed and styled markdown
};

/// @brief A tooltip component that displays styled text content.
///
/// Tooltip supports both plain text and markdown content, with automatic
/// scrolling for large content. It renders with a border and can be
/// positioned anywhere on the screen using the overlay system.
///
/// Features:
/// - Plain text or markdown content
/// - Automatic line wrapping
/// - Scrollable content with scroll indicators
/// - Mouse wheel scrolling support
/// - Configurable maximum size
class Tooltip: public Component
{
  public:
    /// @brief Constructs an empty tooltip framed in single lines.
    Tooltip() = default;

    /// @brief Constructs an empty tooltip framed in @p border.
    /// @param border The frame's style.
    explicit Tooltip(BorderStyle border);

    ~Tooltip() override = default;

    /// @brief Returns the style the tooltip is framed in.
    [[nodiscard]] BorderStyle border() const noexcept { return _border; }

    // --- Content ---

    /// @brief Sets the tooltip content.
    /// @param text The content text.
    /// @param type Content type (plain text or markdown).
    void setContent(std::string_view text, TooltipContentType type = TooltipContentType::PlainText);

    /// @brief Returns the current content.
    [[nodiscard]] std::string_view content() const noexcept { return _rawContent; }

    /// @brief Returns the content type.
    [[nodiscard]] TooltipContentType contentType() const noexcept { return _contentType; }

    /// @brief Clears the content.
    void clear();

    // --- Size ---

    /// @brief Sets the maximum display size before scrolling.
    /// @param maxSize Maximum width and height.
    void setMaxSize(Size maxSize);

    /// @brief Returns the maximum size.
    [[nodiscard]] Size maxSize() const noexcept { return _maxSize; }

    // --- Scrolling ---

    /// @brief Scrolls up by the given number of lines.
    void scrollUp(int lines = 1);

    /// @brief Scrolls down by the given number of lines.
    void scrollDown(int lines = 1);

    /// @brief Scrolls to the top.
    void scrollToTop();

    /// @brief Scrolls to the bottom.
    void scrollToBottom();

    /// @brief Returns true if can scroll up.
    [[nodiscard]] bool canScrollUp() const noexcept;

    /// @brief Returns true if can scroll down.
    [[nodiscard]] bool canScrollDown() const noexcept;

    /// @brief Returns the current scroll offset.
    [[nodiscard]] int scrollOffset() const noexcept { return _scrollOffset; }

    // --- Component Interface ---

    void render(Canvas& canvas) override;

    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    [[nodiscard]] Size preferredSize() const override;

    /// @brief Tooltips don't take focus.
    [[nodiscard]] bool focusable() const override { return false; }

  private:
    BorderStyle _border = BorderStyle::Single;
    std::string _rawContent;
    TooltipContentType _contentType = TooltipContentType::PlainText;
    StyledText _styledContent;
    Size _maxSize = { .width = 60, .height = 15 };
    int _scrollOffset = 0;

    // Border and padding
    static constexpr int BorderWidth = 1;
    static constexpr int Padding = 1;

    /// @brief Reparses the content after settings change.
    void parseContent();

    /// @brief Returns the content area size (excluding border/padding).
    [[nodiscard]] Size contentAreaSize() const;

    /// @brief Renders scroll indicators if needed.
    void renderScrollIndicators(Canvas& canvas) const;
};

} // namespace core::tui
