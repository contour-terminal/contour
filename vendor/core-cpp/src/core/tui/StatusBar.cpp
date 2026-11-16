// SPDX-License-Identifier: Apache-2.0
#include <core/tui/StatusBar.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Theme.hpp>

#include <algorithm>
#include <ranges>

namespace core::tui
{

// =============================================================================
// Component Interface
// =============================================================================

void StatusBar::render(Canvas& canvas)
{
    auto const width = canvas.width();

    // Fill background
    canvas.fill(canvas.area(), ' ', _style.background);

    auto col = 1;

    // If we have hints, render them on the left
    if (!_hints.empty())
    {
        auto first = true;
        for (auto const& hint: _hints)
        {
            if (!first)
            {
                col += canvas.putString(0, col, _style.separatorChar, _style.separator);
            }
            first = false;

            col += canvas.putString(0, col, hint.key, _style.keyStyle);
            col += canvas.putString(0, col, " ", _style.actionStyle);
            col += canvas.putString(0, col, hint.action, _style.actionStyle);
        }
    }
    else if (!_leftText.empty())
    {
        canvas.putString(0, 1, _leftText, _style.actionStyle);
    }

    // Center text
    if (!_centerText.empty())
    {
        auto const centerCol = (width - static_cast<int>(_centerText.size())) / 2;
        canvas.putString(0, centerCol, _centerText, _style.actionStyle);
    }

    // Right text
    if (!_rightText.empty())
    {
        auto const rightCol = width - static_cast<int>(_rightText.size()) - 1;
        canvas.putString(0, rightCol, _rightText, _style.actionStyle);
    }
}

Size StatusBar::preferredSize() const
{
    return { .width = 80, .height = 1 }; // Full width (will be adjusted), 1 row
}

void StatusBar::setHints(std::vector<KeyHint> hints)
{
    _hints = std::move(hints);
}

void StatusBar::addHint(std::string key, std::string action)
{
    _hints.push_back(KeyHint { .key = std::move(key), .action = std::move(action) });
}

void StatusBar::clearHints()
{
    _hints.clear();
}

void StatusBar::setLeftText(std::string text)
{
    _leftText = std::move(text);
}

void StatusBar::setCenterText(std::string text)
{
    _centerText = std::move(text);
}

void StatusBar::setRightText(std::string text)
{
    _rightText = std::move(text);
}

void StatusBar::setStyle(StatusBarStyle style)
{
    _style = style;
}

auto StatusBar::style() const noexcept -> StatusBarStyle const&
{
    return _style;
}

auto StatusBar::formatHints() const -> std::string
{
    auto result = std::string {};
    auto first = true;

    for (auto const& hint: _hints)
    {
        if (!first)
            result += _style.separatorChar;
        first = false;

        result += hint.key;
        result += " ";
        result += hint.action;
    }

    return result;
}

auto StatusBar::hintsWidth() const -> int
{
    auto width = 0;

    for (auto const i: std::views::iota(std::size_t { 0 }, _hints.size()))
    {
        if (i > 0)
            width += static_cast<int>(_style.separatorChar.size());

        width += static_cast<int>(_hints[i].key.size());
        width += 1; // space
        width += static_cast<int>(_hints[i].action.size());
    }

    return width;
}

auto defaultStatusBarStyle() -> StatusBarStyle
{
    auto style = StatusBarStyle {};

    // Dark background
    style.background.bg = static_cast<std::uint8_t>(236); // Dark gray
    style.background.fg = static_cast<std::uint8_t>(252); // Light gray

    // Keys in bold/accent color
    style.keyStyle.bold = true;
    style.keyStyle.fg = 0x82B4FF_rgb; // Light blue

    // Actions in normal color
    style.actionStyle.fg = static_cast<std::uint8_t>(252); // Light gray

    // Separator dim
    style.separator.dim = true;

    return style;
}

} // namespace core::tui
