// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace contour::session
{

/// How much of a paste its permission prompt shows.
struct PastePreviewLimits
{
    std::size_t lines = 2;          ///< How many lines are shown, from the top.
    std::size_t bytesPerLine = 160; ///< How many bytes of UTF-8 a shown line keeps, before its ellipsis.
};

/// The top of a paste, made safe and short enough to show: what a large paste's prompt says it pastes.
struct PastePreview
{
    std::vector<std::string> lines {}; ///< The shown lines, sanitized for display, each cut to its limit.
    std::size_t moreLines = 0;         ///< How many lines of the paste are not shown.
};

namespace detail
{
    /// The longest prefix of @p text of at most @p maxBytes that ends before a UTF-8 lead byte, so no
    /// character is split. A run of continuation bytes too long to back out of is cut where the limit falls.
    [[nodiscard]] inline std::string_view cutAtCharacter(std::string_view text, std::size_t maxBytes) noexcept
    {
        if (text.size() <= maxBytes)
            return text;
        auto const isContinuation = [](char c) noexcept {
            return (static_cast<unsigned char>(c) & 0b1100'0000U) == 0b1000'0000U;
        };
        auto end = maxBytes;
        while (end > 0 && isContinuation(text[end]))
            --end;
        return text.substr(0, end == 0 ? maxBytes : end);
    }

    /// The ellipsis a cut line ends with.
    inline constexpr auto Ellipsis = std::string_view { "…" };
} // namespace detail

/// Makes the preview a large paste's prompt shows: its first lines, made safe to show and each bounded,
/// and how many lines follow them.
///
/// One pass over @p text, copying only the shown prefixes. A trailing LF opens no line. A shown line is
/// sanitized for display (controls become U+FFFD) from a bounded prefix -- never the whole line -- and the
/// result cut again at a character boundary, so a line of controls, which grows as it is sanitized, still
/// keeps its bound; a line that lost anything ends with "…".
/// @param text The paste, as it would be sent.
/// @param limits How much of it is shown.
/// @return The preview.
[[nodiscard]] inline PastePreview makePastePreview(std::string_view text, PastePreviewLimits limits = {})
{
    auto preview = PastePreview {};
    auto rest = text;
    while (!rest.empty())
    {
        auto const end = rest.find('\n');
        auto const line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view {} : rest.substr(end + 1);

        if (preview.lines.size() == limits.lines)
        {
            // Counted, not copied: one line for this one, one for every LF still ahead, and one more for a
            // last line no LF ends.
            auto const lineFeeds = static_cast<std::size_t>(std::ranges::count(rest, '\n'));
            preview.moreLines = 1 + lineFeeds + ((!rest.empty() && rest.back() != '\n') ? 1 : 0);
            break;
        }

        auto const bounded = detail::cutAtCharacter(line, limits.bytesPerLine);
        auto const sanitized = vtbackend::sanitizeCommandLine(bounded, vtbackend::SanitizePurpose::Display);
        auto const shown = detail::cutAtCharacter(sanitized, limits.bytesPerLine);
        auto shownLine = std::string { shown };
        if (bounded.size() < line.size() || shown.size() < sanitized.size())
            shownLine += detail::Ellipsis;
        preview.lines.push_back(std::move(shownLine));
    }
    return preview;
}

/// A byte count as a prompt says it: "1 byte", "999 bytes", "524.3 kB", "2.4 MB" (decimal units).
/// @param bytes The count.
/// @return Its spelling.
[[nodiscard]] inline std::string formatByteCount(std::size_t bytes)
{
    constexpr auto Kilo = 1000.0;
    constexpr auto Mega = Kilo * Kilo;
    if (bytes == 1)
        return "1 byte";
    if (static_cast<double>(bytes) < Kilo)
        return std::format("{} bytes", bytes);
    // Decided on the figure as it is printed, to one decimal: 999 950 bytes would print as "1000.0 kB".
    auto const kilobytes = static_cast<double>(bytes) / Kilo;
    if (std::round(kilobytes * 10.0) < Kilo * 10.0)
        return std::format("{:.1f} kB", kilobytes);
    return std::format("{:.1f} MB", static_cast<double>(bytes) / Mega);
}

} // namespace contour::session
