// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Tooltip.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Theme.hpp>

#include <algorithm>
#include <ranges>

namespace core::tui
{

Tooltip::Tooltip(BorderStyle border): _border(border)
{
}

void Tooltip::setContent(std::string_view text, TooltipContentType type)
{
    _rawContent = std::string(text);
    _contentType = type;
    _scrollOffset = 0;
    parseContent();
}

void Tooltip::clear()
{
    _rawContent.clear();
    _styledContent.clear();
    _scrollOffset = 0;
}

void Tooltip::setMaxSize(Size maxSize)
{
    _maxSize = maxSize;
    parseContent(); // Re-wrap content for new width
}

void Tooltip::scrollUp(int lines)
{
    _scrollOffset = std::max(0, _scrollOffset - lines);
}

void Tooltip::scrollDown(int lines)
{
    int const maxScroll = std::max(0, _styledContent.lineCount() - contentAreaSize().height);
    _scrollOffset = std::min(maxScroll, _scrollOffset + lines);
}

void Tooltip::scrollToTop()
{
    _scrollOffset = 0;
}

void Tooltip::scrollToBottom()
{
    _scrollOffset = std::max(0, _styledContent.lineCount() - contentAreaSize().height);
}

bool Tooltip::canScrollUp() const noexcept
{
    return _scrollOffset > 0;
}

bool Tooltip::canScrollDown() const noexcept
{
    return _scrollOffset < _styledContent.lineCount() - contentAreaSize().height;
}

void Tooltip::render(Canvas& canvas)
{
    if (_styledContent.empty())
        return;

    auto const size = preferredSize();
    auto const contentArea = contentAreaSize();

    // Get theme colors
    auto const& themeRef = canvas.theme();
    Style boxStyle;
    boxStyle.bg = themeRef.colors.overlay;
    boxStyle.fg = themeRef.colors.text;

    // Fill background
    canvas.fill(Rect { .x = 0, .y = 0, .width = size.width, .height = size.height }, ' ', boxStyle);

    canvas.drawBox(Rect { .x = 0, .y = 0, .width = size.width, .height = size.height }, _border, boxStyle);

    // Render content
    auto contentCanvas = canvas.subcanvas(Rect { .x = BorderWidth + Padding,
                                                 .y = BorderWidth,
                                                 .width = contentArea.width,
                                                 .height = contentArea.height });
    _styledContent.renderTo(contentCanvas, _scrollOffset, contentArea.height);

    // Render scroll indicators
    renderScrollIndicators(canvas);
}

EventResult Tooltip::onEvent(InputEvent const& event)
{
    // Handle mouse scroll events
    if (auto const* mouse = std::get_if<MouseEvent>(&event))
    {
        if (mouse->type == MouseEvent::Type::ScrollUp)
        {
            scrollUp(3);
            invalidate();
            return EventResult::Handled;
        }
        if (mouse->type == MouseEvent::Type::ScrollDown)
        {
            scrollDown(3);
            invalidate();
            return EventResult::Handled;
        }
    }
    return EventResult::Ignored;
}

Size Tooltip::preferredSize() const
{
    if (_styledContent.empty())
        return { .width = 0, .height = 0 };

    int const contentWidth = _styledContent.maxLineWidth();
    int const contentHeight = _styledContent.lineCount();

    // Add border and padding
    int const totalWidth = contentWidth + (2 * (BorderWidth + Padding));
    int const totalHeight = contentHeight + (2 * BorderWidth);

    // Clamp to max size
    return { .width = std::min(totalWidth, _maxSize.width),
             .height = std::min(totalHeight, _maxSize.height) };
}

void Tooltip::parseContent()
{
    if (_rawContent.empty())
    {
        _styledContent.clear();
        return;
    }

    // Calculate content width for wrapping
    int const wrapWidth = _maxSize.width - (2 * (BorderWidth + Padding));

    if (_contentType == TooltipContentType::Markdown)
    {
        _styledContent = StyledText::fromMarkdown(_rawContent, wrapWidth);
    }
    else
    {
        _styledContent = StyledText::fromPlainText(_rawContent, wrapWidth);
    }
}

Size Tooltip::contentAreaSize() const
{
    auto const pref = preferredSize();
    return { .width = std::max(0, pref.width - (2 * (BorderWidth + Padding))),
             .height = std::max(0, pref.height - (2 * BorderWidth)) };
}

void Tooltip::renderScrollIndicators(Canvas& canvas) const
{
    auto const size = preferredSize();
    auto const& themeRef = canvas.theme();

    Style indicatorStyle;
    indicatorStyle.fg = themeRef.colors.accent;
    indicatorStyle.bg = themeRef.colors.overlay;

    // Show scroll-up indicator
    if (canScrollUp())
    {
        canvas.put(1, size.width - 2, "\xe2\x96\xb2", indicatorStyle); // U+25B2 BLACK UP-POINTING TRIANGLE
    }

    // Show scroll-down indicator
    if (canScrollDown())
    {
        canvas.put(size.height - 2,
                   size.width - 2,
                   "\xe2\x96\xbc",
                   indicatorStyle); // U+25BC BLACK DOWN-POINTING TRIANGLE
    }
}

} // namespace core::tui
