// SPDX-License-Identifier: Apache-2.0
#include <contour/session/HyperlinkTooltip.hpp>

#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/links/LinkIndex.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace contour::session
{

namespace
{
    constexpr auto Ellipsis = std::string_view { "…" };

    /// Whether @p byte continues a multi-byte UTF-8 sequence rather than starting one.
    [[nodiscard]] constexpr bool isContinuation(char byte) noexcept
    {
        return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
    }

    /// The byte offset of every codepoint in @p text, plus a final offset of text.size().
    ///
    /// Built once rather than scanned twice, so head and tail are both cut on a boundary the same walk
    /// established. Malformed input degrades gracefully: a stray continuation byte simply joins the
    /// codepoint before it.
    [[nodiscard]] std::vector<size_t> codepointOffsets(std::string_view text)
    {
        auto offsets = std::vector<size_t> {};
        for (auto const i: std::views::iota(size_t { 0 }, text.size()))
            if (!isContinuation(text[i]))
                offsets.push_back(i);
        offsets.push_back(text.size());
        return offsets;
    }

    /// How a file:// URL begins, in the lower case vtbackend::parseFileUrl reads.
    constexpr auto FileScheme = std::string_view { "file://" };

    /// Whether @p uri is a file:// URL, its scheme spelled in any case (RFC 3986 §3.1).
    [[nodiscard]] bool isFileUrl(std::string_view uri) noexcept
    {
        return uri.size() >= FileScheme.size()
               && std::ranges::equal(uri.substr(0, FileScheme.size()), FileScheme, [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) == b;
                  });
    }

    /// The path a file:// URI names on THIS host, decoded for reading, or nullopt when it names anything
    /// else -- or when no file:// URI names a path here (@p localHost nullopt). A truncated or malformed
    /// escape is left as written rather than dropped: showing the user what the application actually sent
    /// beats silently swallowing it (@see vtbackend::parseFileUrl).
    [[nodiscard]] std::optional<std::string> localFilePath(std::string_view uri,
                                                           std::optional<std::string_view> localHost)
    {
        if (!isFileUrl(uri) || !localHost)
            return std::nullopt;

        // The scheme is case-insensitive (RFC 3986 §3.1); the URL reader expects it in lower case.
        return vtbackend::localPathOfFileUrl(
            std::string { FileScheme } + std::string { uri.substr(FileScheme.size()) }, *localHost);
    }

    /// What separates two parts of a breadcrumb: U+203A between spaces.
    constexpr auto BreadcrumbSeparator = std::string_view { " \u203A " };

    /// Whether @p text is an RFC 3986 scheme: a letter, then letters, digits, "+", "-" or ".".
    [[nodiscard]] constexpr bool isScheme(std::string_view text) noexcept
    {
        auto const isAlpha = [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        };
        auto const isSchemeChar = [&](char ch) {
            return isAlpha(ch) || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
        };
        return !text.empty() && isAlpha(text.front()) && std::ranges::all_of(text, isSchemeChar);
    }

    /// @p uri as a breadcrumb, its host and its non-empty decoded path segments, or nullopt where it has no
    /// authority to read one from (mailto:, a bare word). Only a URI that opens with a scheme and "://" has
    /// one: a "://" anywhere else (a mailto: body, a data: payload) is text, and reading it as an authority
    /// would show a host the link does not open and hide the scheme that says so.
    [[nodiscard]] std::optional<std::string> breadcrumbOf(std::string_view uri)
    {
        constexpr auto AuthorityMark = std::string_view { "://" };
        auto const schemeEnd = uri.find(':');
        if (schemeEnd == std::string_view::npos || !isScheme(uri.substr(0, schemeEnd))
            || uri.substr(schemeEnd, AuthorityMark.size()) != AuthorityMark)
            return std::nullopt;

        // The query and the fragment go, then the authority's user info and port.
        auto rest = uri.substr(schemeEnd + AuthorityMark.size());
        rest = rest.substr(0, rest.find_first_of("?#"));
        auto const pathStart = rest.find('/');
        auto authority = rest.substr(0, pathStart);
        auto const path = pathStart == std::string_view::npos ? std::string_view {} : rest.substr(pathStart);
        if (auto const at = authority.rfind('@'); at != std::string_view::npos)
            authority = authority.substr(at + 1);
        // A port follows the last colon, unless that colon sits inside a bracketed IPv6 address.
        if (auto const colon = authority.rfind(':');
            colon != std::string_view::npos && authority.find(']', colon) == std::string_view::npos)
            authority = authority.substr(0, colon);

        auto parts = std::vector<std::string> {};
        if (!authority.empty())
            parts.emplace_back(authority);
        for (auto const segment: path | std::views::split('/'))
            if (!std::ranges::empty(segment))
                parts.push_back(core::unescapeURL(std::string_view { segment.begin(), segment.end() }));

        auto crumb = std::string {};
        for (auto const& part: parts)
        {
            if (!crumb.empty())
                crumb += BreadcrumbSeparator;
            crumb += part;
        }
        return crumb;
    }

    /// @p text made safe to show and elided to @p maxLength codepoints.
    [[nodiscard]] std::string shown(std::string_view text, size_t maxLength)
    {
        return elideMiddle(vtbackend::sanitizeCommandLine(text, vtbackend::SanitizePurpose::Display),
                           maxLength);
    }
} // namespace

std::string elideMiddle(std::string_view text, size_t maxLength)
{
    auto const offsets = codepointOffsets(text);
    auto const length = offsets.size() - 1;
    if (length <= maxLength)
        return std::string { text };

    // Too short to hold anything but the ellipsis itself.
    if (maxLength <= 1)
        return std::string { Ellipsis };

    auto const keep = maxLength - 1;
    auto const head = (keep + 1) / 2; // the head keeps the odd codepoint: scheme and host read first
    auto const tail = keep - head;

    auto out = std::string { text.substr(0, offsets[head]) };
    out += Ellipsis;
    out += text.substr(offsets[length - tail]);
    return out;
}

std::string linkPillText(std::string_view uri, std::optional<std::string_view> localHost, size_t maxLength)
{
    if (uri.empty())
        return {};

    // A local file is its path. A file elsewhere keeps its scheme: the path alone would read as one on this
    // machine.
    if (auto const path = localFilePath(uri, localHost))
        return shown(*path, maxLength);
    if (isFileUrl(uri))
        return shown(uri, maxLength);
    return shown(breadcrumbOf(uri).value_or(std::string { uri }), maxLength);
}

std::string linkPillText(vtbackend::DetectedLink const& link, size_t maxLength)
{
    if (link.target.kind != vtbackend::LinkKind::Path)
        return linkPillText(link.target.target, std::nullopt, maxLength);

    auto path = link.target.target;
    if (link.target.location)
        path += std::format(":{}:{}", link.target.location->line, link.target.location->column);
    return shown(path, maxLength);
}

HyperlinkHoverTracker::Change HyperlinkHoverTracker::update(HoveredLink const& link, size_t maxLength)
{
    if (std::holds_alternative<std::monostate>(link))
        return clear();

    // Still the same link: say nothing, so the pill is not re-decided for each cell the pointer traces.
    if (link == _link)
        return {};

    _link = link;
    auto text = std::visit(
        [&](auto const& hovered) -> std::string {
            using Hovered = std::decay_t<decltype(hovered)>;
            if constexpr (std::is_same_v<Hovered, OscLinkTarget>)
                return linkPillText(hovered.uri, _localHost(), maxLength);
            else if constexpr (std::is_same_v<Hovered, DetectedLinkText>)
                return shown(hovered.text,
                             maxLength); // formatted already, but shown safe and short all the same
            else
                return {};
        },
        link);
    return { .change = HoverChange::Changed, .text = std::move(text) };
}

HyperlinkHoverTracker::Change HyperlinkHoverTracker::clear()
{
    if (std::holds_alternative<std::monostate>(_link))
        return {};

    _link = std::monostate {};
    return { .change = HoverChange::Changed, .text = {} };
}

} // namespace contour::session
