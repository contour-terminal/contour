// SPDX-License-Identifier: Apache-2.0
#include <contour/session/HyperlinkTooltip.hpp>

#include <vtbackend/FileUrl.hpp>

#include <crispy/Utils.hpp>

#include <optional>
#include <ranges>
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

    /// The path a file:// URI names on THIS host, decoded for reading, or nullopt when it names anything
    /// else. A truncated or malformed escape is left as written rather than dropped: showing the user what
    /// the application actually sent beats silently swallowing it (@see crispy::unescapeURL).
    [[nodiscard]] std::optional<std::string> localFilePath(std::string_view uri, std::string_view localHost)
    {
        if (!uri.starts_with("file://"))
            return std::nullopt;

        return vtbackend::localWorkingDirectory(std::string { uri }, localHost)
            .transform([](std::string const& path) { return crispy::unescapeURL(path); });
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

std::string hyperlinkTooltipText(std::string_view uri, std::string_view localHost, size_t maxLength)
{
    if (uri.empty())
        return {};

    return elideMiddle(localFilePath(uri, localHost).value_or(std::string { uri }), maxLength);
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
    return { .changed = true, .text = hyperlinkTooltipText(uri, _localHost, maxLength), .anchor = _anchor };
}

HyperlinkHoverTracker::Change HyperlinkHoverTracker::clear()
{
    if (_uri.empty())
        return {};

    _uri.clear();
    return { .changed = true, .text = {}, .anchor = _anchor };
}

} // namespace contour::session
