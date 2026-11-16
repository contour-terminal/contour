// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Box.hpp>

#include <algorithm>
#include <ranges>

namespace core::tui
{

auto BorderChars::fromStyle(BorderStyle style) noexcept -> BorderChars
{
    switch (style)
    {
        case BorderStyle::None:
            return BorderChars {
                .horizontal = " ",
                .vertical = " ",
                .topLeft = " ",
                .topRight = " ",
                .bottomLeft = " ",
                .bottomRight = " ",
                .leftT = " ",
                .rightT = " ",
                .topT = " ",
                .bottomT = " ",
                .cross = " ",
            };
        case BorderStyle::Single:
            return BorderChars {
                .horizontal = "\u2500",  // ─
                .vertical = "\u2502",    // │
                .topLeft = "\u250C",     // ┌
                .topRight = "\u2510",    // ┐
                .bottomLeft = "\u2514",  // └
                .bottomRight = "\u2518", // ┘
                .leftT = "\u251C",       // ├
                .rightT = "\u2524",      // ┤
                .topT = "\u252C",        // ┬
                .bottomT = "\u2534",     // ┴
                .cross = "\u253C",       // ┼
            };
        case BorderStyle::Double:
            return BorderChars {
                .horizontal = "\u2550",  // ═
                .vertical = "\u2551",    // ║
                .topLeft = "\u2554",     // ╔
                .topRight = "\u2557",    // ╗
                .bottomLeft = "\u255A",  // ╚
                .bottomRight = "\u255D", // ╝
                .leftT = "\u2560",       // ╠
                .rightT = "\u2563",      // ╣
                .topT = "\u2566",        // ╦
                .bottomT = "\u2569",     // ╩
                .cross = "\u256C",       // ╬
            };
        case BorderStyle::Rounded:
            return BorderChars {
                .horizontal = "\u2500",  // ─
                .vertical = "\u2502",    // │
                .topLeft = "\u256D",     // ╭
                .topRight = "\u256E",    // ╮
                .bottomLeft = "\u2570",  // ╰
                .bottomRight = "\u256F", // ╯
                .leftT = "\u251C",       // ├
                .rightT = "\u2524",      // ┤
                .topT = "\u252C",        // ┬
                .bottomT = "\u2534",     // ┴
                .cross = "\u253C",       // ┼
            };
        case BorderStyle::Heavy:
            return BorderChars {
                .horizontal = "\u2501",  // ━
                .vertical = "\u2503",    // ┃
                .topLeft = "\u250F",     // ┏
                .topRight = "\u2513",    // ┓
                .bottomLeft = "\u2517",  // ┗
                .bottomRight = "\u251B", // ┛
                .leftT = "\u2523",       // ┣
                .rightT = "\u252B",      // ┫
                .topT = "\u2533",        // ┳
                .bottomT = "\u253B",     // ┻
                .cross = "\u254B",       // ╋
            };
        case BorderStyle::Dashed:
            return BorderChars {
                .horizontal = "\u254C",  // ╌
                .vertical = "\u254E",    // ╎
                .topLeft = "\u250C",     // ┌
                .topRight = "\u2510",    // ┐
                .bottomLeft = "\u2514",  // └
                .bottomRight = "\u2518", // ┘
                .leftT = "\u251C",       // ├
                .rightT = "\u2524",      // ┤
                .topT = "\u252C",        // ┬
                .bottomT = "\u2534",     // ┴
                .cross = "\u253C",       // ┼
            };
    }
    return fromStyle(BorderStyle::Single);
}

Box::Box(BoxConfig config): _config(std::move(config))
{
}

void Box::render(TerminalOutput& output) const
{
    if (_config.width < 2 || _config.height < 2)
        return;

    auto const chars = BorderChars::fromStyle(_config.border);

    renderTopBorder(output, chars);
    renderSideBorders(output, chars);
    renderBottomBorder(output, chars);

    if (_config.fillBackground)
        clearContent(output);
}

void Box::renderTopBorder(TerminalOutput& output, BorderChars const& chars) const
{
    output.moveTo(_config.row, _config.col);
    output.writeText(chars.topLeft, _config.borderStyle);

    auto const innerWidth = _config.width - 2;

    if (_config.title && !_config.title->empty())
    {
        auto const& title = *_config.title;
        auto const titleLen = static_cast<int>(title.size());
        auto const maxTitleLen = std::max(0, innerWidth - 2); // Leave at least 1 border char each side

        auto displayTitle = title;
        if (titleLen > maxTitleLen)
            displayTitle = title.substr(0, static_cast<std::size_t>(maxTitleLen));

        auto const actualTitleLen = static_cast<int>(displayTitle.size());
        auto leftPad = 0;
        auto rightPad = 0;

        switch (_config.titleAlign)
        {
            case TitleAlign::Left:
                leftPad = 1;
                rightPad = innerWidth - actualTitleLen - 1;
                break;
            case TitleAlign::Center:
                leftPad = (innerWidth - actualTitleLen) / 2;
                rightPad = innerWidth - actualTitleLen - leftPad;
                break;
            case TitleAlign::Right:
                rightPad = 1;
                leftPad = innerWidth - actualTitleLen - 1;
                break;
        }

        // Left padding with horizontal line
        for ([[maybe_unused]] auto const column: std::views::iota(0, std::max(0, leftPad)))
            output.writeText(chars.horizontal, _config.borderStyle);

        // Title
        output.writeText(displayTitle, _config.titleStyle);

        // Right padding with horizontal line
        for ([[maybe_unused]] auto const column: std::views::iota(0, std::max(0, rightPad)))
            output.writeText(chars.horizontal, _config.borderStyle);
    }
    else
    {
        // No title, just horizontal line
        for ([[maybe_unused]] auto const column: std::views::iota(0, std::max(0, innerWidth)))
            output.writeText(chars.horizontal, _config.borderStyle);
    }

    output.writeText(chars.topRight, _config.borderStyle);
}

void Box::renderBottomBorder(TerminalOutput& output, BorderChars const& chars) const
{
    auto const bottomRow = _config.row + _config.height - 1;
    output.moveTo(bottomRow, _config.col);
    output.writeText(chars.bottomLeft, _config.borderStyle);

    auto const innerWidth = _config.width - 2;
    for ([[maybe_unused]] auto const column: std::views::iota(0, std::max(0, innerWidth)))
        output.writeText(chars.horizontal, _config.borderStyle);

    output.writeText(chars.bottomRight, _config.borderStyle);
}

void Box::renderSideBorders(TerminalOutput& output, BorderChars const& chars) const
{
    auto const rightCol = _config.col + _config.width - 1;

    auto const firstRow = _config.row + 1;
    auto const lastRow = _config.row + _config.height - 1;
    for (auto const row: std::views::iota(firstRow, std::max(firstRow, lastRow)))
    {
        output.moveTo(row, _config.col);
        output.writeText(chars.vertical, _config.borderStyle);
        output.moveTo(row, rightCol);
        output.writeText(chars.vertical, _config.borderStyle);
    }
}

auto Box::innerWidth() const noexcept -> int
{
    auto const borderWidth = (_config.border == BorderStyle::None) ? 0 : 2;
    return std::max(0, _config.width - borderWidth - _config.paddingLeft - _config.paddingRight);
}

auto Box::innerHeight() const noexcept -> int
{
    auto const borderHeight = (_config.border == BorderStyle::None) ? 0 : 2;
    return std::max(0, _config.height - borderHeight - _config.paddingTop - _config.paddingBottom);
}

auto Box::contentStartRow() const noexcept -> int
{
    auto const borderOffset = (_config.border == BorderStyle::None) ? 0 : 1;
    return _config.row + borderOffset + _config.paddingTop;
}

auto Box::contentStartCol() const noexcept -> int
{
    auto const borderOffset = (_config.border == BorderStyle::None) ? 0 : 1;
    return _config.col + borderOffset + _config.paddingLeft;
}

void Box::setConfig(BoxConfig config)
{
    _config = std::move(config);
}

auto Box::config() const noexcept -> BoxConfig const&
{
    return _config;
}

void Box::clearContent(TerminalOutput& output) const
{
    auto const startRow = contentStartRow();
    auto const startCol = contentStartCol();
    auto const width = innerWidth();
    auto const height = innerHeight();

    auto const spaces = std::string(static_cast<std::size_t>(width), ' ');

    for (auto const row: std::views::iota(startRow, startRow + std::max(0, height)))
    {
        output.moveTo(row, startCol);
        output.writeText(spaces, _config.backgroundStyle);
    }
}

void renderBox(TerminalOutput& output, BoxConfig const& config)
{
    auto box = Box(config);
    box.render(output);
}

} // namespace core::tui
