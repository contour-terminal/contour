// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace core::tui
{

/// @brief Horizontal alignment requested by a GitHub-style HTML block.
enum class HtmlAlign : std::uint8_t
{
    Left,   ///< Default; no padding.
    Center, ///< Padded so the content sits mid-line.
    Right,  ///< Padded so the content ends at the right margin.
};

/// @brief A parsed HTML tag such as `<div align="center">` or `</p>`.
struct HtmlTag
{
    std::string name;       ///< Lowercased tag name.
    std::string_view attrs; ///< Raw attribute text between the name and '>'.
    bool isClosing;         ///< True for `</tag>`.
    std::size_t endPos;     ///< Index just past the closing '>'.
};

/// @brief An HTML block tag the markdown renderer understands.
struct HtmlBlockTagDef
{
    std::string_view name; ///< Lowercased tag name.
    int headingLevel;      ///< 1-6 when the tag is a heading, 0 otherwise.
    bool alwaysCenters;    ///< True for `<center>`, which centers without an align attribute.
};

/// @brief An image reference extracted from a line that contains nothing else.
struct StandaloneImage
{
    std::string alt;            ///< Alt text.
    std::string src;            ///< Source path or URL as written.
    std::optional<int> widthPx; ///< Explicit pixel width from an HTML `width=`, if any.
};

/// @brief Removes leading and trailing ASCII whitespace (spaces, tabs, carriage returns).
/// @param text The text to trim.
/// @return The trimmed view.
[[nodiscard]] auto trimAscii(std::string_view text) -> std::string_view;

/// @brief Case-insensitive search for @p needle in @p haystack.
/// @param haystack The text to search.
/// @param needle The text to find.
/// @return Index of the match, or std::string_view::npos.
[[nodiscard]] auto findCaseInsensitive(std::string_view haystack, std::string_view needle) -> std::size_t;

/// @brief Parses an HTML tag beginning at @p pos, which must be '<'.
///
/// Quoted attribute values may contain '>', which is respected when locating
/// the tag's end.
///
/// @param text The text being parsed.
/// @param pos Index of the opening '<'.
/// @return The parsed tag, or std::nullopt when the syntax is not a tag.
[[nodiscard]] auto parseHtmlTag(std::string_view text, std::size_t pos) -> std::optional<HtmlTag>;

/// @brief Extracts an attribute value from a tag's raw attribute text.
///
/// Handles single quotes, double quotes and unquoted values, with a
/// case-insensitive attribute name. A name that is only a suffix of a longer
/// attribute does not match.
///
/// @param attrs The raw attribute text.
/// @param name The attribute name to find (lowercase).
/// @return The value, or std::nullopt when absent.
[[nodiscard]] auto htmlAttr(std::string_view attrs, std::string_view name) -> std::optional<std::string_view>;

/// @brief Maps an HTML `align` attribute value to an alignment.
/// @param value The attribute value, in any case.
/// @return The alignment, or std::nullopt when the value is unrecognised.
[[nodiscard]] auto parseHtmlAlign(std::string_view value) -> std::optional<HtmlAlign>;

/// @brief Looks up a block tag definition by lowercased name.
/// @param name The tag name.
/// @return The definition, or nullptr when the tag is not an alignment block.
[[nodiscard]] auto findHtmlBlockTag(std::string_view name) -> HtmlBlockTagDef const*;

/// @brief Parses an HTML `width` attribute value in pixels.
///
/// Accepts a bare integer or an `NNpx` suffix. Percentages and malformed values
/// yield std::nullopt, meaning "auto-fit".
///
/// @param value The attribute value.
/// @return The width in pixels, or std::nullopt.
[[nodiscard]] auto parseWidthPx(std::string_view value) -> std::optional<int>;

/// @brief Detects a line whose entire content is a single image.
///
/// Matches both `![alt](src)` and `<img src="..." [width="..."] [alt="..."]>`.
/// Only such lines are eligible for block placement as a Sixel image; an image
/// sharing a line with other text renders as alt text.
///
/// @param line The source line.
/// @return The image, or std::nullopt when the line is not a lone image.
[[nodiscard]] auto detectStandaloneImage(std::string_view line) -> std::optional<StandaloneImage>;

/// @brief Rewrites the inline HTML GitHub READMEs use into equivalent markdown.
///
/// `<a href="u">t</a>` becomes `[t](u)`, `<img alt="a">` becomes its alt text,
/// `<b>`/`<strong>` become `**` and `<i>`/`<em>` become `*`. Unknown tags are
/// dropped, matching how a browser renders them.
///
/// @param text The inner HTML text.
/// @return The equivalent markdown.
[[nodiscard]] auto translateInlineHtml(std::string_view text) -> std::string;

} // namespace core::tui
