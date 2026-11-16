// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <optional>
#include <ranges>
#include <string_view>

namespace core::tui
{

/// @brief Result of finding a CommonMark inline code span.
struct InlineCodeSpan
{
    std::string_view content; ///< The code content between backtick sequences.
    std::size_t endPos;       ///< Position in the text after the closing backtick sequence.
};

/// @brief Applies CommonMark space stripping to inline code content.
///
/// If the content has both a leading and trailing space, and is not entirely
/// spaces, strips one leading and one trailing space.
///
/// @param content The raw content between backtick sequences.
/// @return The content with CommonMark space stripping applied.
[[nodiscard]] constexpr auto stripInlineCodeSpaces(std::string_view content) noexcept -> std::string_view
{
    if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ')
    {
        auto allSpaces = true;
        for (auto ch: content)
        {
            if (ch != ' ')
            {
                allSpaces = false;
                break;
            }
        }
        if (!allSpaces)
            return content.substr(1, content.size() - 2);
    }
    return content;
}

/// @brief Finds the end of a CommonMark inline code span starting at pos.
///
/// Counts the opening backtick sequence length N starting at pos, then searches
/// for a matching closing sequence of exactly N backticks.
///
/// @param text The full text being parsed.
/// @param pos Position of the first opening backtick.
/// @return The code span content and end position, or std::nullopt if no match found.
[[nodiscard]] constexpr auto findInlineCodeEnd(std::string_view text, std::size_t pos)
    -> std::optional<InlineCodeSpan>
{
    auto const tickStart = pos;
    while (pos < text.size() && text[pos] == '`')
        ++pos;
    auto const tickCount = pos - tickStart;

    auto searchPos = pos;
    while (searchPos < text.size())
    {
        auto const closeStart = text.find('`', searchPos);
        if (closeStart == std::string_view::npos)
            break;

        auto closeEnd = closeStart;
        while (closeEnd < text.size() && text[closeEnd] == '`')
            ++closeEnd;

        if (closeEnd - closeStart == tickCount)
        {
            auto const content = text.substr(pos, closeStart - pos);
            return InlineCodeSpan { .content = stripInlineCodeSpaces(content), .endPos = closeEnd };
        }
        searchPos = closeEnd;
    }

    return std::nullopt;
}

/// @brief A parsed CommonMark inline link or image span.
struct LinkSpan
{
    std::string_view label; ///< Raw label text; may itself contain an image span or inline markdown.
    std::string_view url;   ///< Destination as written (may be relative, absolute, or a fragment).
    std::string_view title; ///< Optional CommonMark title, without its quotes; empty when absent.
    std::size_t endPos;     ///< Position in the text just past the closing ')'.
    bool isImage;           ///< True when the span was introduced by '!' (an image).
};

/// @brief Whether @p c is CommonMark inline whitespace.
[[nodiscard]] constexpr auto isInlineSpace(char c) noexcept -> bool
{
    return c == ' ' || c == '\t';
}

/// @brief Finds a `[label](url)` or `![label](url)` span.
///
/// The label's closing bracket is located by bracket-depth counting, so a nested
/// image span such as `[![alt](img.png)](target)` yields the full outer label
/// `![alt](img.png)` rather than stopping at the inner `]`.
///
/// The destination is separated from an optional CommonMark title, so
/// `![alt](shot.png "Screenshot")` yields the url `shot.png`, not
/// `shot.png "Screenshot"`. An angle-bracketed destination `(<my file.png>)`
/// may contain spaces.
///
/// @param text The full text being parsed.
/// @param pos Position of the introducing '!' or '['.
/// @return The parsed span, or std::nullopt when the syntax is incomplete.
[[nodiscard]] constexpr auto findLinkSpan(std::string_view text, std::size_t pos) -> std::optional<LinkSpan>
{
    auto const isImage = pos < text.size() && text[pos] == '!';
    auto const bracket = isImage ? pos + 1 : pos;
    if (bracket >= text.size() || text[bracket] != '[')
        return std::nullopt;

    // Locate the matching ']' by counting nested brackets.
    auto depth = 1;
    auto scan = bracket + 1;
    while (scan < text.size() && depth > 0)
    {
        if (text[scan] == '[')
            ++depth;
        else if (text[scan] == ']')
            --depth;
        if (depth > 0)
            ++scan;
    }
    if (depth != 0 || scan + 1 >= text.size() || text[scan + 1] != '(')
        return std::nullopt;

    // Find the closing ')', ignoring one inside a quoted title.
    auto closeParen = std::string_view::npos;
    auto quote = '\0';
    for (auto const i: std::views::iota(scan + 2, std::max(scan + 2, text.size())))
    {
        auto const c = text[i];
        if (quote != '\0')
        {
            if (c == quote)
                quote = '\0';
        }
        else if (c == '"' || c == '\'')
        {
            quote = c;
        }
        else if (c == ')')
        {
            closeParen = i;
            break;
        }
    }
    if (closeParen == std::string_view::npos)
        return std::nullopt;

    // Split the parenthesised part into a destination and an optional title.
    auto inside = text.substr(scan + 2, closeParen - scan - 2);
    while (!inside.empty() && isInlineSpace(inside.front()))
        inside.remove_prefix(1);
    while (!inside.empty() && isInlineSpace(inside.back()))
        inside.remove_suffix(1);

    auto url = inside;
    auto title = std::string_view {};
    if (!inside.empty() && inside.front() == '<')
    {
        // <dest> may contain spaces.
        if (auto const close = inside.find('>'); close != std::string_view::npos)
        {
            url = inside.substr(1, close - 1);
            title = inside.substr(close + 1);
        }
    }
    else if (auto const space = inside.find_first_of(" \t"); space != std::string_view::npos)
    {
        url = inside.substr(0, space);
        title = inside.substr(space + 1);
    }

    // Strip surrounding whitespace and quotes from the title.
    while (!title.empty() && isInlineSpace(title.front()))
        title.remove_prefix(1);
    while (!title.empty() && isInlineSpace(title.back()))
        title.remove_suffix(1);
    if (title.size() >= 2 && (title.front() == '"' || title.front() == '\'') && title.back() == title.front())
        title = title.substr(1, title.size() - 2);

    return LinkSpan { .label = text.substr(bracket + 1, scan - bracket - 1),
                      .url = url,
                      .title = title,
                      .endPos = closeParen + 1,
                      .isImage = isImage };
}

/// @brief Whether @p url is an absolute, terminal-clickable URI.
///
/// Relative paths (`LICENSE`) and fragments (`#install`) are not clickable in a
/// terminal, so they must not be wrapped in an OSC-8 hyperlink.
///
/// @param url The destination to classify.
/// @return true when the URL carries a scheme worth hyperlinking.
[[nodiscard]] constexpr auto isAbsoluteUrl(std::string_view url) -> bool
{
    return url.contains("://") || url.starts_with("mailto:") || url.starts_with("tel:");
}

/// @brief Whether @p text looks like a bare email address (for `<a@b.com>` autolinks).
/// @param text The candidate text, without angle brackets.
/// @return true when it contains an '@' with non-empty sides and a dot in the domain.
[[nodiscard]] constexpr auto looksLikeEmail(std::string_view text) -> bool
{
    auto const at = text.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= text.size())
        return false;
    if (text.contains(':') || text.contains(' '))
        return false;
    return text.find('.', at + 1) != std::string_view::npos;
}

/// @brief Whether the text between `<` and `>` is a CommonMark autolink target.
///
/// An autolink contains no whitespace, quotes or angle brackets — which is what
/// distinguishes `<https://x.com>` from an HTML tag such as
/// `<a href="https://x.com">`, whose attribute value also contains `://`.
///
/// @param text The text between the angle brackets.
/// @return true when @p text should be rendered as a clickable autolink.
[[nodiscard]] constexpr auto isAutolinkTarget(std::string_view text) -> bool
{
    if (text.empty() || text.find_first_of(" \t\"'<>") != std::string_view::npos)
        return false;
    return isAbsoluteUrl(text) || looksLikeEmail(text);
}

} // namespace core::tui
