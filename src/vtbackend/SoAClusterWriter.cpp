// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/SoAClusterWriter.h>

#include <libunicode/convert.h>
#include <libunicode/grapheme_segmenter.h>
#include <libunicode/utf8.h>
#include <libunicode/width.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string_view>

namespace vtbackend
{

namespace
{

    /// Fast path: write pure ASCII text directly to SoA arrays.
    /// Bulk-fills SGR/width/hyperlink arrays instead of per-cell writeCellToSoA.
    /// Avoids the per-cell trivial flag check (sets it ONCE at the end).
    ///
    /// When writing to a freshly-reset trivial line with matching attributes,
    /// skips redundant widths/sgr/hyperlinks fills (they already hold correct values).
    size_t writeAsciiToSoA(LineSoA& line,
                           size_t startCol,
                           std::string_view text,
                           GraphicsAttributes const& attrs,
                           HyperlinkId hyperlink) noexcept
    {
        auto const maxCols = line.codepoints.size();
        if (startCol >= maxCols)
            return 0;
        auto const count = std::min(text.size(), maxCols - startCol);

        // Widen ASCII bytes to char32_t codepoints (always required)
        for (size_t i = 0; i < count; ++i)
            line.codepoints[startCol + i] = static_cast<char32_t>(text[i]);

        // clusterSize: always required (reset fills with 0, we need 1)
        std::fill_n(line.clusterSize.data() + startCol, count, uint8_t { 1 });

        // Skip redundant fills when writing to a freshly-reset trivial line
        // with matching attributes. The arrays already contain correct values
        // from the most recent resetLine/clearRange.
        auto const skipFills =
            line.trivial && (startCol == 0) && (attrs == line.fillAttrs) && (hyperlink == HyperlinkId {});

        if (!skipFills)
        {
            std::fill_n(line.widths.data() + startCol, count, uint8_t { 1 });
            std::fill_n(line.sgr.data() + startCol, count, attrs);
            std::fill_n(line.hyperlinks.data() + startCol, count, hyperlink);
        }

        // Trivial flag: all cells got the same SGR. Check once against cell 0.
        if (line.trivial && startCol > 0 && (attrs != line.sgr[0] || hyperlink != line.hyperlinks[0]))
        {
            line.trivial = false;
        }

        return count;
    }

    /// Slow path: decode UTF-8, segment grapheme clusters, write to SoA.
    /// Handles multi-codepoint clusters, wide characters, and combining marks.
    size_t writeNonAsciiToSoA(LineSoA& line,
                              size_t startCol,
                              std::string_view text,
                              GraphicsAttributes const& attrs,
                              HyperlinkId hyperlink) noexcept
    {
        auto const maxCols = line.codepoints.size();
        auto col = startCol;

        unicode::utf8_decoder_state utf8State {};
        unicode::grapheme_segmenter_state graphemeState {};
        char32_t prevCodepoint = 0;

        for (auto const byte: text)
        {
            auto const result = unicode::from_utf8(utf8State, static_cast<uint8_t>(byte));

            if (std::holds_alternative<unicode::Incomplete>(result))
                continue;

            if (std::holds_alternative<unicode::Invalid>(result))
            {
                // Invalid UTF-8: write replacement character
                if (col < maxCols)
                {
                    writeCellToSoA(line, col, U'\uFFFD', 1, attrs, hyperlink);
                    ++col;
                }
                utf8State = {};
                graphemeState = {};
                prevCodepoint = 0;
                continue;
            }

            auto const codepoint = std::get<unicode::Success>(result).value;
            auto const charWidth = static_cast<uint8_t>(std::max(1u, unicode::width(codepoint)));

            // Determine if this codepoint starts a new grapheme cluster
            bool const startsNewCluster = [&] {
                if (!prevCodepoint)
                {
                    unicode::grapheme_process_init(codepoint, graphemeState);
                    return true;
                }
                return unicode::grapheme_process_breakable(codepoint, graphemeState);
            }();

            if (startsNewCluster)
            {
                // Start a new cell
                if (col >= maxCols)
                    break;

                writeCellToSoA(line, col, codepoint, charWidth, attrs, hyperlink);

                // Fill continuation cells for wide characters
                if (charWidth > 1)
                    fillWideCharContinuation(line, col + 1, charWidth - 1, attrs, hyperlink);

                col += charWidth;
            }
            else
            {
                // Continuation codepoint — append to previous cell's grapheme cluster
                if (col > startCol)
                {
                    auto const prevCol = col - 1;
                    // Walk back to find the actual cell (skip continuation cells)
                    auto targetCol = prevCol;
                    while (targetCol > startCol
                           && line.sgr[targetCol].flags.contains(CellFlag::WideCharContinuation))
                        --targetCol;

                    appendCodepointToCluster(line, targetCol, codepoint);
                }
            }

            prevCodepoint = codepoint;
        }

        return col - startCol;
    }

} // namespace

size_t writeTextToSoA(LineSoA& line,
                      size_t startCol,
                      std::string_view text,
                      GraphicsAttributes const& attrs,
                      HyperlinkId hyperlink,
                      bool asciiHint)
{
    if (text.empty())
        return 0;

    // Fast path: caller confirmed ASCII
    if (asciiHint)
        return writeAsciiToSoA(line, startCol, text, attrs, hyperlink);

    // Slow path: mixed/non-ASCII — full UTF-8 decode + grapheme cluster segmentation
    return writeNonAsciiToSoA(line, startCol, text, attrs, hyperlink);
}

} // namespace vtbackend
