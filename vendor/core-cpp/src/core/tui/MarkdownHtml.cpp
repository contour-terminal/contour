// SPDX-License-Identifier: Apache-2.0
#include <core/tui/MarkdownHtml.hpp>

#include <core/tui/MarkdownInline.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

namespace core::tui
{

namespace
{
    /// @brief Lowercases an ASCII string.
    auto toLowerAscii(std::string_view text) -> std::string
    {
        auto result = std::string(text);
        std::ranges::transform(
            result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    /// @brief The block tags GitHub honors for alignment, declared once.
    constexpr HtmlBlockTagDef HtmlBlockTags[] = {
        { .name = "div", .headingLevel = 0, .alwaysCenters = false },
        { .name = "p", .headingLevel = 0, .alwaysCenters = false },
        { .name = "center", .headingLevel = 0, .alwaysCenters = true },
        { .name = "h1", .headingLevel = 1, .alwaysCenters = false },
        { .name = "h2", .headingLevel = 2, .alwaysCenters = false },
        { .name = "h3", .headingLevel = 3, .alwaysCenters = false },
        { .name = "h4", .headingLevel = 4, .alwaysCenters = false },
        { .name = "h5", .headingLevel = 5, .alwaysCenters = false },
        { .name = "h6", .headingLevel = 6, .alwaysCenters = false },
    };
} // namespace

/// @brief Removes leading and trailing ASCII whitespace.
auto trimAscii(std::string_view text) -> std::string_view
{
    auto const isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r';
    };
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

/// @brief Case-insensitive search for @p needle in @p haystack.
/// @return Index of the match, or std::string_view::npos.
auto findCaseInsensitive(std::string_view haystack, std::string_view needle) -> std::size_t
{
    if (needle.empty() || needle.size() > haystack.size())
        return std::string_view::npos;
    auto const equalsIgnoreCase = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    // Keep the subrange rather than binding its iterator to a variable: std::string_view's
    // iterator is a raw pointer in libstdc++ but a class type in MSVC's STL, so no single
    // spelling of `auto` satisfies both clang-tidy's readability-qualified-auto and clang-cl.
    auto const match = std::ranges::search(haystack, needle, equalsIgnoreCase);
    if (match.empty())
        return std::string_view::npos;
    return static_cast<std::size_t>(std::ranges::distance(haystack.begin(), match.begin()));
}

/// @brief Parses an HTML tag beginning at @p pos (which must be '<').
///
/// Quoted attribute values may contain '>', which is respected when locating
/// the tag's end.
///
/// @return The parsed tag, or std::nullopt when the syntax is not a tag.
auto parseHtmlTag(std::string_view text, std::size_t pos) -> std::optional<HtmlTag>
{
    if (pos >= text.size() || text[pos] != '<')
        return std::nullopt;

    auto cursor = pos + 1;
    auto const isClosing = cursor < text.size() && text[cursor] == '/';
    if (isClosing)
        ++cursor;

    auto const nameStart = cursor;
    while (cursor < text.size() && (std::isalnum(static_cast<unsigned char>(text[cursor])) != 0))
        ++cursor;
    if (cursor == nameStart)
        return std::nullopt;
    auto const name = toLowerAscii(text.substr(nameStart, cursor - nameStart));

    auto const attrStart = cursor;
    auto quote = '\0';
    while (cursor < text.size())
    {
        auto const c = text[cursor];
        if (quote != '\0')
        {
            if (c == quote)
                quote = '\0';
        }
        else if (c == '"' || c == '\'')
        {
            quote = c;
        }
        else if (c == '>')
        {
            break;
        }
        ++cursor;
    }
    if (cursor >= text.size())
        return std::nullopt;

    return HtmlTag { .name = name,
                     .attrs = text.substr(attrStart, cursor - attrStart),
                     .isClosing = isClosing,
                     .endPos = cursor + 1 };
}

/// @brief Extracts an attribute value from a tag's raw attribute text.
///
/// Handles single quotes, double quotes and unquoted values, with a
/// case-insensitive attribute name.
///
/// @param attrs The raw attribute text.
/// @param name The attribute name to find (lowercase).
/// @return The value, or std::nullopt when absent.
auto htmlAttr(std::string_view attrs, std::string_view name) -> std::optional<std::string_view>
{
    auto searchFrom = std::size_t { 0 };
    while (searchFrom < attrs.size())
    {
        auto const at = findCaseInsensitive(attrs.substr(searchFrom), name);
        if (at == std::string_view::npos)
            return std::nullopt;
        auto pos = searchFrom + at;
        searchFrom = pos + name.size();

        // The name must stand alone (not a suffix of another attribute).
        if (pos > 0
            && (std::isalnum(static_cast<unsigned char>(attrs[pos - 1])) != 0 || attrs[pos - 1] == '-'))
            continue;

        auto cursor = pos + name.size();
        while (cursor < attrs.size() && attrs[cursor] == ' ')
            ++cursor;
        if (cursor >= attrs.size() || attrs[cursor] != '=')
            continue;
        ++cursor;
        while (cursor < attrs.size() && attrs[cursor] == ' ')
            ++cursor;
        if (cursor >= attrs.size())
            return std::nullopt;

        if (attrs[cursor] == '"' || attrs[cursor] == '\'')
        {
            auto const quote = attrs[cursor];
            auto const valueStart = cursor + 1;
            auto const valueEnd = attrs.find(quote, valueStart);
            if (valueEnd == std::string_view::npos)
                return std::nullopt;
            return attrs.substr(valueStart, valueEnd - valueStart);
        }

        auto const valueStart = cursor;
        while (cursor < attrs.size() && attrs[cursor] != ' ' && attrs[cursor] != '/')
            ++cursor;
        return attrs.substr(valueStart, cursor - valueStart);
    }
    return std::nullopt;
}

/// @brief Maps an HTML `align` attribute value to an alignment.
/// @return The alignment, or std::nullopt when the value is unrecognised.
auto parseHtmlAlign(std::string_view value) -> std::optional<HtmlAlign>
{
    auto const lower = toLowerAscii(value);
    if (lower == "center")
        return HtmlAlign::Center;
    if (lower == "right")
        return HtmlAlign::Right;
    if (lower == "left")
        return HtmlAlign::Left;
    return std::nullopt;
}

/// @brief Looks up a block tag definition by name.
/// @return The definition, or nullptr when the tag is not an alignment block.
auto findHtmlBlockTag(std::string_view name) -> HtmlBlockTagDef const*
{
    auto const* const it = std::ranges::find(HtmlBlockTags, name, &HtmlBlockTagDef::name);
    return it != std::ranges::end(HtmlBlockTags) ? it : nullptr;
}

/// @brief Parses an HTML `width` attribute value in pixels.
///
/// Accepts a bare integer or an `NNpx` suffix. Percentages yield nullopt
/// (auto-fit), as do malformed values.
auto parseWidthPx(std::string_view value) -> std::optional<int>
{
    if (value.ends_with("px"))
        value.remove_suffix(2);

    auto width = 0;
    auto const [end, ec] = std::from_chars(value.data(), value.data() + value.size(), width);
    if (ec != std::errc {} || end != value.data() + value.size() || width <= 0)
        return std::nullopt;
    return width;
}

/// @brief Detects a line whose entire content is a single image.
///
/// Matches both `![alt](src)` and `<img src="..." [width="..."] [alt="..."]>`.
/// Only such lines are eligible for Sixel block placement; an image sharing a
/// line with other text renders as alt text through renderInline().
auto detectStandaloneImage(std::string_view line) -> std::optional<StandaloneImage>
{
    auto const trimmed = trimAscii(line);
    if (trimmed.empty())
        return std::nullopt;

    if (trimmed.front() == '!')
    {
        auto const span = findLinkSpan(trimmed, 0);
        if (span && span->isImage && span->endPos == trimmed.size())
            return StandaloneImage { .alt = std::string(span->label),
                                     .src = std::string(span->url),
                                     .widthPx = std::nullopt };
        return std::nullopt;
    }

    if (trimmed.front() == '<')
    {
        auto const tag = parseHtmlTag(trimmed, 0);
        if (!tag || tag->isClosing || tag->name != "img" || tag->endPos != trimmed.size())
            return std::nullopt;
        auto const src = htmlAttr(tag->attrs, "src");
        if (!src)
            return std::nullopt;
        return StandaloneImage {
            .alt = std::string(htmlAttr(tag->attrs, "alt").value_or(std::string_view {})),
            .src = std::string(*src),
            .widthPx = htmlAttr(tag->attrs, "width").and_then(parseWidthPx),
        };
    }

    return std::nullopt;
}

/// @brief Rewrites the inline HTML GitHub READMEs use into equivalent markdown.
///
/// `<a href="u">t</a>` becomes `[t](u)`, `<img alt="a">` becomes its alt text,
/// and `<b>/<strong>` and `<i>/<em>` become `**` and `*`. Unknown tags are dropped,
/// matching how a browser renders them.
auto translateInlineHtml(std::string_view text) -> std::string
{
    auto result = std::string {};
    result.reserve(text.size());

    auto pos = std::size_t { 0 };
    while (pos < text.size())
    {
        if (text[pos] != '<')
        {
            result += text[pos++];
            continue;
        }

        auto const tag = parseHtmlTag(text, pos);
        if (!tag)
        {
            result += text[pos++];
            continue;
        }

        if (tag->name == "a" && !tag->isClosing)
        {
            auto const href = htmlAttr(tag->attrs, "href");
            auto const close = findCaseInsensitive(text.substr(tag->endPos), "</a>");
            if (href && close != std::string_view::npos)
            {
                auto const label = text.substr(tag->endPos, close);
                result += std::format("[{}]({})", translateInlineHtml(label), *href);
                pos = tag->endPos + close + 4;
                continue;
            }
        }
        else if (tag->name == "img" && !tag->isClosing)
        {
            result += htmlAttr(tag->attrs, "alt").value_or(std::string_view {});
            pos = tag->endPos;
            continue;
        }
        else if (tag->name == "b" || tag->name == "strong")
        {
            result += "**";
            pos = tag->endPos;
            continue;
        }
        else if (tag->name == "i" || tag->name == "em")
        {
            result += "*";
            pos = tag->endPos;
            continue;
        }

        // Unknown or closing tag: drop it.
        pos = tag->endPos;
    }
    return result;
}

} // namespace core::tui
