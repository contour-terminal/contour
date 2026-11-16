// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Text.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>

namespace core::tui
{

// =============================================================================
// TextLine
// =============================================================================

auto TextLine::width() const noexcept -> int
{
    auto total = 0;
    for (auto const& span: spans)
        total += displayWidth(span.text);
    return total;
}

void TextLine::append(std::string text, Style const& style)
{
    spans.push_back(TextSpan { .text = std::move(text), .style = style });
}

void TextLine::append(std::string_view text)
{
    spans.push_back(TextSpan { .text = std::string(text), .style = {} });
}

// =============================================================================
// Text
// =============================================================================

Text::Text(std::string_view content, Style const& style)
{
    setText(content, style);
}

void Text::setText(std::string_view content, Style const& style)
{
    _lines.clear();

    // Split by newlines
    auto start = std::size_t { 0 };
    while (start < content.size())
    {
        auto const end = content.find('\n', start);
        auto line = TextLine {};

        if (end == std::string_view::npos)
        {
            line.append(std::string(content.substr(start)), style);
            start = content.size();
        }
        else
        {
            line.append(std::string(content.substr(start, end - start)), style);
            start = end + 1;
        }

        _lines.push_back(std::move(line));
    }

    // Ensure at least one empty line
    if (_lines.empty())
        _lines.emplace_back();
}

void Text::setLines(std::vector<TextLine> lines)
{
    _lines = std::move(lines);
}

auto Text::text() const -> std::string
{
    auto result = std::string {};

    for (auto const i: std::views::iota(std::size_t { 0 }, _lines.size()))
    {
        if (i > 0)
            result += '\n';

        for (auto const& span: _lines[i].spans)
            result += span.text;
    }

    return result;
}

auto Text::lines() const noexcept -> std::vector<TextLine> const&
{
    return _lines;
}

void Text::setAlign(TextAlign align)
{
    _align = align;
}

auto Text::align() const noexcept -> TextAlign
{
    return _align;
}

void Text::setWrapMode(WrapMode mode)
{
    _wrapMode = mode;
}

auto Text::wrapMode() const noexcept -> WrapMode
{
    return _wrapMode;
}

void Text::setMaxWidth(int width)
{
    _maxWidth = width;
}

auto Text::maxWidth() const noexcept -> int
{
    return _maxWidth;
}

void Text::render(TerminalOutput& output, int startRow, int startCol, int width) const
{
    auto const wrapped = wrap(width);

    for (auto const i: std::views::iota(std::size_t { 0 }, wrapped.size()))
    {
        auto const& line = wrapped[i];
        auto const row = startRow + static_cast<int>(i);
        auto const lineWidth = line.width();

        // Calculate starting column based on alignment
        auto col = startCol;
        switch (_align)
        {
            case TextAlign::Left: col = startCol; break;
            case TextAlign::Center: col = startCol + ((width - lineWidth) / 2); break;
            case TextAlign::Right: col = startCol + width - lineWidth; break;
        }

        output.moveTo(row, col);

        for (auto const& span: line.spans)
            output.writeText(span.text, span.style);
    }
}

auto Text::lineCount(int width) const -> int
{
    return static_cast<int>(wrap(width).size());
}

auto Text::wrap(int width) const -> std::vector<TextLine>
{
    if (_wrapMode == WrapMode::None || width <= 0)
        return _lines;

    auto result = std::vector<TextLine> {};

    for (auto const& inputLine: _lines)
    {
        // Combine all spans into a single string for wrapping
        auto fullText = std::string {};
        auto defaultStyle = Style {};

        for (auto const& span: inputLine.spans)
        {
            fullText += span.text;
            if (fullText.size() == span.text.size())
                defaultStyle = span.style;
        }

        if (displayWidth(fullText) <= width)
        {
            result.push_back(inputLine);
            continue;
        }

        // Need to wrap
        if (_wrapMode == WrapMode::Word)
        {
            auto const wrapped = wordWrap(fullText, width);
            for (auto const& wrappedLine: wrapped)
            {
                auto line = TextLine {};
                line.append(wrappedLine, defaultStyle);
                result.push_back(std::move(line));
            }
        }
        else
        {
            // Character-level wrapping
            auto pos = std::size_t { 0 };
            while (pos < fullText.size())
            {
                auto lineLen = std::size_t { 0 };
                auto lineWidth = 0;

                while (pos + lineLen < fullText.size() && lineWidth < width)
                {
                    auto const ch = static_cast<unsigned char>(fullText[pos + lineLen]);
                    // Skip UTF-8 continuation bytes for width calculation
                    if ((ch & 0xC0) != 0x80)
                        ++lineWidth;
                    ++lineLen;
                }

                auto line = TextLine {};
                line.append(fullText.substr(pos, lineLen), defaultStyle);
                result.push_back(std::move(line));

                pos += lineLen;
            }
        }
    }

    return result;
}

// =============================================================================
// Free functions
// =============================================================================

auto wordWrap(std::string_view text, int width) -> std::vector<std::string>
{
    auto result = std::vector<std::string> {};

    if (text.empty() || width <= 0)
    {
        result.emplace_back();
        return result;
    }

    auto currentLine = std::string {};
    auto currentWidth = 0;
    auto wordStart = std::size_t { 0 };

    auto const flushWord = [&](std::size_t wordEnd) {
        if (wordStart >= wordEnd)
            return;

        auto word = std::string(text.substr(wordStart, wordEnd - wordStart));
        auto const wordWidth = displayWidth(word);

        if (currentLine.empty())
        {
            // First word on line - always add it even if too long
            currentLine = std::move(word);
            currentWidth = wordWidth;
        }
        else if (currentWidth + 1 + wordWidth <= width)
        {
            // Word fits on current line
            currentLine += ' ';
            currentLine += word;
            currentWidth += 1 + wordWidth;
        }
        else
        {
            // Start new line
            result.push_back(std::move(currentLine));
            currentLine = std::move(word);
            currentWidth = wordWidth;
        }
    };

    for (auto const i: std::views::iota(std::size_t { 0 }, text.size()))
    {
        auto const ch = text[i];

        if (ch == '\n')
        {
            flushWord(i);
            result.push_back(std::move(currentLine));
            currentLine.clear();
            currentWidth = 0;
            wordStart = i + 1;
        }
        else if (std::isspace(static_cast<unsigned char>(ch)))
        {
            flushWord(i);
            wordStart = i + 1;
        }
    }

    // Flush remaining word
    flushWord(text.size());

    if (!currentLine.empty() || result.empty())
        result.push_back(std::move(currentLine));

    return result;
}

auto truncate(std::string_view text, int width) -> std::string
{
    if (width <= 0)
        return "";

    auto const textWidth = displayWidth(text);
    if (textWidth <= width)
        return std::string(text);

    // Need to truncate. Named, because a braced initializer would pick
    // std::initializer_list<char> rather than the fill constructor.
    if (width <= 3)
    {
        auto const dots = std::string(static_cast<std::size_t>(width), '.');
        return dots;
    }

    auto const targetWidth = width - 1; // Leave room for ellipsis
    auto result = std::string {};
    auto currentWidth = 0;

    for (auto const byte: text)
    {
        if (currentWidth >= targetWidth)
            break;
        result += byte;

        // Only count non-continuation bytes for width
        if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80)
            ++currentWidth;
    }

    result += "\u2026"; // …
    return result;
}

auto displayWidth(std::string_view text) -> int
{
    auto width = 0;

    for (auto const i: std::views::iota(std::size_t { 0 }, text.size()))
    {
        auto const ch = static_cast<unsigned char>(text[i]);

        // Skip UTF-8 continuation bytes
        if ((ch & 0xC0) == 0x80)
            continue;

        // TODO: Use libunicode for proper width calculation (wcwidth equivalent)
        // For now, assume 1 cell per codepoint (which is wrong for wide chars like CJK)
        ++width;
    }

    return width;
}

} // namespace core::tui
