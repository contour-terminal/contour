// SPDX-License-Identifier: Apache-2.0
#include <core/tui/MarkdownTable.hpp>

#include <core/tui/MarkdownHtml.hpp>
#include <core/tui/MarkdownInline.hpp>
#include <core/tui/Unicode.hpp>

#include <libunicode/utf8_grapheme_segmenter.h>

#include <algorithm>
#include <ranges>

namespace core::tui
{

namespace
{
    /// @brief Trims leading and trailing whitespace from a string_view.
    auto trim(std::string_view sv) -> std::string_view
    {
        auto const start = sv.find_first_not_of(" \t");
        if (start == std::string_view::npos)
            return {};
        auto const end = sv.find_last_not_of(" \t");
        return sv.substr(start, end - start + 1);
    }

    /// @brief Calculates display width of a string using grapheme cluster segmentation.
    auto displayWidth(std::string_view text) -> int
    {
        auto width = 0;
        auto segmenter = unicode::utf8_grapheme_segmenter(text);
        for (auto const& cluster: segmenter)
            width += graphemeClusterWidth(cluster);
        return width;
    }
} // namespace

auto detectTableRow(std::string_view line) -> bool
{
    auto const trimmed = trim(line);
    return !trimmed.empty() && trimmed.front() == '|';
}

auto detectTableSeparator(std::string_view line) -> bool
{
    auto const trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() != '|')
        return false;

    // Must contain at least one cell with dashes
    auto pos = std::size_t { 1 }; // skip leading |
    auto foundCell = false;

    while (pos < trimmed.size())
    {
        // Find next |
        auto const pipePos = trimmed.find('|', pos);
        auto const cellEnd = (pipePos != std::string_view::npos) ? pipePos : trimmed.size();
        auto const cell = trim(trimmed.substr(pos, cellEnd - pos));

        if (!cell.empty())
        {
            // Cell must match pattern: :?-+:?
            auto cellPos = std::size_t { 0 };
            if (cellPos < cell.size() && cell[cellPos] == ':')
                ++cellPos;

            auto const dashStart = cellPos;
            while (cellPos < cell.size() && cell[cellPos] == '-')
                ++cellPos;

            if (cellPos == dashStart)
                return false; // no dashes found

            if (cellPos < cell.size() && cell[cellPos] == ':')
                ++cellPos;

            if (cellPos != cell.size())
                return false; // unexpected characters

            foundCell = true;
        }

        if (pipePos == std::string_view::npos)
            break;
        pos = pipePos + 1;
    }

    return foundCell;
}

auto splitTableRow(std::string_view line) -> std::vector<std::string>
{
    auto const trimmed = trim(line);
    auto result = std::vector<std::string> {};

    if (trimmed.empty() || trimmed.front() != '|')
        return result;

    // Strip leading and optional trailing pipe
    auto content = trimmed.substr(1);
    if (!content.empty() && content.back() == '|')
        content.remove_suffix(1);

    auto pos = std::size_t { 0 };
    while (pos <= content.size())
    {
        auto const pipePos = content.find('|', pos);
        auto const cellEnd = (pipePos != std::string_view::npos) ? pipePos : content.size();
        auto const cell = trim(content.substr(pos, cellEnd - pos));
        result.emplace_back(cell);
        if (pipePos == std::string_view::npos)
            break;
        pos = pipePos + 1;
    }

    return result;
}

auto parseTableAlignments(std::string_view line) -> std::vector<TableAlignment>
{
    auto const cells = splitTableRow(line);
    auto result = std::vector<TableAlignment> {};
    result.reserve(cells.size());

    for (auto const& cell: cells)
    {
        auto const trimmed = trim(cell);
        if (trimmed.empty())
        {
            result.push_back(TableAlignment::Left);
            continue;
        }

        auto const leftColon = trimmed.front() == ':';
        auto const rightColon = trimmed.back() == ':';

        if (leftColon && rightColon)
            result.push_back(TableAlignment::Center);
        else if (rightColon)
            result.push_back(TableAlignment::Right);
        else
            result.push_back(TableAlignment::Left);
    }

    return result;
}

auto computeColumnWidths(ParsedTable const& table) -> std::vector<int>
{
    auto widths = std::vector<int>(table.columnCount, 3); // minimum width 3

    for (auto const col: std::views::iota(std::size_t { 0 }, table.columnCount))
    {
        if (col < table.headers.size())
            widths[col] = std::max(widths[col], inlineDisplayWidth(table.headers[col]));

        for (auto const& row: table.rows)
        {
            if (col < row.size())
                widths[col] = std::max(widths[col], inlineDisplayWidth(row[col]));
        }
    }

    return widths;
}

auto alignCell(std::string_view text, int width, TableAlignment alignment) -> std::string
{
    auto const textWidth = displayWidth(text);
    auto const padding = std::max(0, width - textWidth);

    switch (alignment)
    {
        case TableAlignment::Right: {
            auto result = std::string(static_cast<std::size_t>(padding), ' ');
            result.append(text);
            return result;
        }
        case TableAlignment::Center: {
            auto const leftPad = padding / 2;
            auto const rightPad = padding - leftPad;
            auto result = std::string(static_cast<std::size_t>(leftPad), ' ');
            result.append(text);
            result.append(static_cast<std::size_t>(rightPad), ' ');
            return result;
        }
        case TableAlignment::Left:
        default: {
            auto result = std::string(text);
            result.append(static_cast<std::size_t>(padding), ' ');
            return result;
        }
    }
}

void constrainColumnWidths(std::vector<int>& widths, int maxTableWidth)
{
    if (widths.empty() || maxTableWidth <= 0)
        return;

    // Border overhead: 1 (left border) + for each column: 2 (padding) + 1 (right border/separator)
    // = 1 + columnCount * 3
    auto const columnCount = static_cast<int>(widths.size());
    auto const overhead = 1 + (columnCount * 3);
    auto const availableContent = maxTableWidth - overhead;

    if (availableContent <= 0)
        return;

    auto totalContent = 0;
    for (auto w: widths)
        totalContent += w;

    if (totalContent <= availableContent)
        return; // Already fits

    // Waterfall algorithm: only shrink the widest columns, preserving narrow ones.
    // Reduced columns merge into the next tier so all equally-wide columns shrink together.
    auto constexpr MinWidth = 3;
    auto excess = totalContent - availableContent;

    // Build sorted indices by width descending.
    auto indices = std::vector<std::size_t>(widths.size());
    for (auto const i: std::views::iota(std::size_t { 0 }, indices.size()))
        indices[i] = i;
    std::ranges::sort(indices, [&](auto a, auto b) { return widths[a] > widths[b]; });

    // Reduce widest columns tier by tier toward the next-widest.
    auto i = std::size_t { 0 };
    while (i < indices.size() && excess > 0)
    {
        // Find how many columns share the current widest tier (including previously reduced).
        auto const tierWidth = widths[indices[i]];
        auto j = i + 1;
        while (j < indices.size() && widths[indices[j]] == tierWidth)
            ++j;
        auto const tierCount = static_cast<int>(j - i);

        // Target: next tier's width, or minWidth if this is the last tier.
        auto const nextTierWidth = (j < indices.size()) ? widths[indices[j]] : MinWidth;
        auto const target = std::max(nextTierWidth, MinWidth);
        auto const reductionPerCol = tierWidth - target;
        auto const maxTierReduction = reductionPerCol * tierCount;

        if (maxTierReduction >= excess)
        {
            // This tier can absorb all remaining excess — distribute evenly.
            auto const perCol = excess / tierCount;
            auto remainder = excess % tierCount;
            for (auto const k: std::views::iota(i, std::max(i, j)))
            {
                auto const extra = (remainder > 0) ? 1 : 0;
                widths[indices[k]] -= perCol + extra;
                if (remainder > 0)
                    --remainder;
            }
            excess = 0;
        }
        else
        {
            // Reduce all columns in this tier to the target width.
            for (auto const k: std::views::iota(i, std::max(i, j)))
                widths[indices[k]] = target;
            excess -= maxTierReduction;

            if (target <= MinWidth)
                break; // Cannot reduce further.

            // Don't advance i — reduced columns merge with the next tier.
            continue;
        }
        i = j;
    }
}

auto wrapText(std::string_view text, int maxWidth) -> std::vector<std::string>
{
    if (maxWidth <= 0)
        return { std::string(text) };

    auto const textW = inlineDisplayWidth(text);
    if (textW <= maxWidth)
        return { std::string(text) };

    auto lines = std::vector<std::string> {};
    auto currentLine = std::string {};
    auto currentWidth = 0;
    auto pos = std::size_t { 0 };

    while (pos < text.size())
    {
        // Find the next word (sequence of non-space characters)
        auto const wordStart = text.find_first_not_of(' ', pos);
        if (wordStart == std::string_view::npos)
            break;

        auto const wordEnd = text.find(' ', wordStart);
        auto const word = text.substr(
            wordStart, wordEnd == std::string_view::npos ? std::string_view::npos : wordEnd - wordStart);
        auto const wordWidth = inlineDisplayWidth(word);

        if (currentLine.empty() && wordWidth > maxWidth)
        {
            // Word exceeds maxWidth — break at grapheme cluster boundaries.
            auto segmenter = unicode::utf8_grapheme_segmenter(word);
            auto chunk = std::string {};
            auto chunkWidth = 0;
            for (auto const& cluster: segmenter)
            {
                auto const clusterW = graphemeClusterWidth(cluster);
                if (chunkWidth + clusterW > maxWidth && !chunk.empty())
                {
                    lines.push_back(std::move(chunk));
                    chunk.clear();
                    chunkWidth = 0;
                }
                for (auto ch: cluster)
                    chunk += static_cast<char>(ch);
                chunkWidth += clusterW;
            }
            if (!chunk.empty())
            {
                currentLine = std::move(chunk);
                currentWidth = chunkWidth;
            }
        }
        else if (currentLine.empty())
        {
            // First word on the line — fits within maxWidth.
            currentLine = std::string(word);
            currentWidth = wordWidth;
        }
        else if (currentWidth + 1 + wordWidth <= maxWidth)
        {
            // Fits on current line with a space separator
            currentLine += ' ';
            currentLine += word;
            currentWidth += 1 + wordWidth;
        }
        else
        {
            // Wrap to next line
            lines.push_back(std::move(currentLine));
            currentLine = std::string(word);
            currentWidth = wordWidth;
        }

        pos = (wordEnd == std::string_view::npos) ? text.size() : wordEnd;
    }

    if (!currentLine.empty())
        lines.push_back(std::move(currentLine));

    if (lines.empty())
        lines.emplace_back();

    return lines;
}

auto stripInlineMarkdown(std::string_view text) -> std::string
{
    auto result = std::string {};
    result.reserve(text.size());
    auto pos = std::size_t { 0 };

    while (pos < text.size())
    {
        // Inline code: `...` or ``...`` (CommonMark multi-backtick)
        if (text[pos] == '`')
        {
            if (auto const span = findInlineCodeEnd(text, pos))
            {
                result.append(span->content);
                pos = span->endPos;
                continue;
            }
        }

        // Bold: **...**
        if (pos + 1 < text.size() && text[pos] == '*' && text[pos + 1] == '*')
        {
            auto const endBold = text.find("**", pos + 2);
            if (endBold != std::string_view::npos)
            {
                result.append(text.substr(pos + 2, endBold - pos - 2));
                pos = endBold + 2;
                continue;
            }
        }

        // Italic: *...*
        if (text[pos] == '*')
        {
            auto const endItalic = text.find('*', pos + 1);
            if (endItalic != std::string_view::npos)
            {
                result.append(text.substr(pos + 1, endItalic - pos - 1));
                pos = endItalic + 1;
                continue;
            }
        }

        // Bold: __...__
        if (pos + 1 < text.size() && text[pos] == '_' && text[pos + 1] == '_')
        {
            auto const endBold = text.find("__", pos + 2);
            if (endBold != std::string_view::npos)
            {
                result.append(text.substr(pos + 2, endBold - pos - 2));
                pos = endBold + 2;
                continue;
            }
        }

        // Link [text](url) or image ![alt](src) — bracket-depth aware so that the
        // badge idiom [![alt](badge.svg)](target) reduces to its alt text.
        if (text[pos] == '[' || text[pos] == '!')
        {
            if (auto const span = findLinkSpan(text, pos))
            {
                // Recursion reduces a nested image label to its alt text, so the
                // badge idiom [![alt](badge.svg)](target) measures as "alt".
                result.append(stripInlineMarkdown(span->label));
                pos = span->endPos;
                continue;
            }
        }

        // Inline HTML anchors/images and CommonMark autolinks — must mirror
        // MarkdownRenderer::renderInline(), or measured and rendered widths
        // disagree and table borders misalign.
        if (text[pos] == '<')
        {
            if (auto const tag = parseHtmlTag(text, pos); tag && !tag->isClosing)
            {
                if (tag->name == "a")
                {
                    auto const close = findCaseInsensitive(text.substr(tag->endPos), "</a>");
                    if (htmlAttr(tag->attrs, "href") && close != std::string_view::npos)
                    {
                        result.append(stripInlineMarkdown(text.substr(tag->endPos, close)));
                        pos = tag->endPos + close + 4;
                        continue;
                    }
                }
                else if (tag->name == "img")
                {
                    result.append(htmlAttr(tag->attrs, "alt").value_or(std::string_view {}));
                    pos = tag->endPos;
                    continue;
                }
            }

            if (auto const close = text.find('>', pos + 1); close != std::string_view::npos)
            {
                if (auto const inner = text.substr(pos + 1, close - pos - 1); isAutolinkTarget(inner))
                {
                    result.append(inner);
                    pos = close + 1;
                    continue;
                }
            }
        }

        // Regular character — find the next special character
        auto const nextSpecial = text.find_first_of("`*_[!<", pos + 1);
        auto const end = (nextSpecial != std::string_view::npos) ? nextSpecial : text.size();
        result.append(text.substr(pos, end - pos));
        pos = end;
    }

    return result;
}

auto inlineDisplayWidth(std::string_view text) -> int
{
    return displayWidth(stripInlineMarkdown(text));
}

auto truncateToDisplayWidth(std::string_view text, int maxWidth) -> std::string
{
    if (maxWidth <= 0)
        return {};

    auto result = std::string {};
    auto width = 0;
    auto segmenter = unicode::utf8_grapheme_segmenter(text);
    for (auto const& cluster: segmenter)
    {
        auto const clusterW = graphemeClusterWidth(cluster);
        if (width + clusterW > maxWidth)
        {
            // Replace last character with ellipsis if possible.
            if (maxWidth >= 1 && !result.empty())
            {
                // Remove last grapheme cluster to make room for ellipsis.
                // Since ellipsis is 1 column wide, trim back if needed.
                while (width > maxWidth - 1 && !result.empty())
                {
                    // Walk back one UTF-8 character at a time to find a grapheme boundary.
                    // Simpler: just pop bytes until we lose enough width.
                    result.pop_back();
                    width = displayWidth(result);
                }
                result += "\xe2\x80\xa6"; // U+2026 HORIZONTAL ELLIPSIS
            }
            return result;
        }
        for (auto ch: cluster)
            result += static_cast<char>(ch);
        width += clusterW;
    }
    return result;
}

} // namespace core::tui
