// SPDX-License-Identifier: Apache-2.0
#include <contour/HyperlinkTooltip.h>

#include <algorithm>
#include <ranges>
#include <vector>

namespace contour
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

    /// Whether @p uri names a path on THIS host, i.e. one whose authority is absent or "localhost".
    [[nodiscard]] bool isLocalFileUri(std::string_view uri)
    {
        constexpr auto Scheme = std::string_view { "file://" };
        if (!uri.starts_with(Scheme))
            return false;
        auto const rest = uri.substr(Scheme.size());
        return rest.starts_with('/') || rest.starts_with("localhost/");
    }

    /// The path part of a local file:// URI, with its percent-escapes decoded.
    [[nodiscard]] std::string decodedLocalPath(std::string_view uri)
    {
        constexpr auto Scheme = std::string_view { "file://" };
        auto rest = uri.substr(Scheme.size());
        if (rest.starts_with("localhost/"))
            rest.remove_prefix(std::string_view { "localhost" }.size());

        auto out = std::string {};
        out.reserve(rest.size());
        for (auto i = size_t { 0 }; i < rest.size(); ++i)
        {
            // A truncated escape at the very end is left as written rather than dropped: showing the
            // user what the application actually sent beats silently swallowing it.
            if (rest[i] == '%' && i + 2 < rest.size())
            {
                auto const hex = rest.substr(i + 1, 2);
                auto const digit = [](char ch) -> int {
                    if (ch >= '0' && ch <= '9')
                        return ch - '0';
                    if (ch >= 'a' && ch <= 'f')
                        return ch - 'a' + 10;
                    if (ch >= 'A' && ch <= 'F')
                        return ch - 'A' + 10;
                    return -1;
                };
                auto const high = digit(hex[0]);
                auto const low = digit(hex[1]);
                if (high >= 0 && low >= 0)
                {
                    out.push_back(static_cast<char>((high * 16) + low));
                    i += 2;
                    continue;
                }
            }
            out.push_back(rest[i]);
        }
        return out;
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

std::string hyperlinkTooltipText(std::string_view uri, size_t maxLength)
{
    if (uri.empty())
        return {};

    return elideMiddle(isLocalFileUri(uri) ? decodedLocalPath(uri) : std::string { uri }, maxLength);
}

HyperlinkHoverTracker::Change HyperlinkHoverTracker::update(std::string_view uri,
                                                            vtbackend::CellLocation cell,
                                                            size_t maxLength)
{
    if (uri.empty())
        return clear();

    // Still the same link: say nothing, so the tooltip's show delay is not restarted, and so the
    // tooltip does not slide along as the pointer traces the text.
    if (uri == _uri)
        return {};

    _uri = std::string { uri };
    _anchor = cell;
    return { .changed = true, .text = hyperlinkTooltipText(uri, maxLength), .anchor = _anchor };
}

HyperlinkHoverTracker::Change HyperlinkHoverTracker::clear()
{
    if (_uri.empty())
        return {};

    _uri.clear();
    return { .changed = true, .text = {}, .anchor = _anchor };
}

} // namespace contour
