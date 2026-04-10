// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/LineSoA.h>

#include <libunicode/convert.h>
#include <libunicode/grapheme_segmenter.h>
#include <libunicode/utf8.h>
#include <libunicode/width.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vtbackend
{

/// Populate a LineSoA from UTF-8 text.
///
/// This is the core write path for the SoA grid. It decodes UTF-8,
/// segments grapheme clusters, and writes all SoA arrays in one pass.
///
/// For pure ASCII text, this is a tight byte → char32_t widening loop.
/// For non-ASCII, it uses libunicode's UTF-8 decoder and grapheme segmenter.
///
/// @param line       Target LineSoA to populate.
/// @param startCol   Starting column offset.
/// @param text       UTF-8 encoded text (typically from scan_text result).
/// @param attrs      Graphics attributes to apply to all written cells.
/// @param hyperlink  Hyperlink ID to apply (default: none).
/// @param asciiHint  When true, skip the internal isAllAscii scan (caller guarantees ASCII).
/// @return Number of columns actually written.
size_t writeTextToSoA(LineSoA& line,
                      size_t startCol,
                      std::string_view text,
                      GraphicsAttributes const& attrs,
                      HyperlinkId hyperlink = {},
                      bool asciiHint = false);

/// Write a single codepoint to a LineSoA cell with full attribute set.
///
/// This is the per-cell write used by writeTextToSoA and directly by
/// the Screen for character-by-character processing (charset mapping, etc.).
inline void writeCellToSoA(LineSoA& line,
                           size_t col,
                           char32_t codepoint,
                           uint8_t width,
                           GraphicsAttributes const& attrs,
                           HyperlinkId hyperlink = {}) noexcept
{
    assert(col < line.codepoints.size());
    auto const oldClusterSize = line.clusterSize[col];
    line.codepoints[col] = codepoint;
    line.widths[col] = width;
    line.sgr[col] = attrs;
    line.hyperlinks[col] = hyperlink;
    line.clusterSize[col] = (codepoint != 0) ? uint8_t { 1 } : uint8_t { 0 };

    // Clear cluster extras if the previous cell had a multi-codepoint grapheme cluster.
    if (oldClusterSize > 1)
        clearClusterExtras(line, col);

    // Invalidate trivial flag if this cell's SGR or hyperlink differs from the first cell's.
    if (line.trivial && col > 0
        && (line.sgr[col] != line.sgr[0] || line.hyperlinks[col] != line.hyperlinks[0]))
    {
        line.trivial = false;
    }
}

/// Fill wide-char continuation cells following a wide character.
inline void fillWideCharContinuation(LineSoA& line,
                                     size_t col,
                                     size_t count,
                                     GraphicsAttributes const& attrs,
                                     HyperlinkId hyperlink = {}) noexcept
{
    auto const contAttrs = GraphicsAttributes { .foregroundColor = attrs.foregroundColor,
                                                .backgroundColor = attrs.backgroundColor,
                                                .underlineColor = attrs.underlineColor,
                                                .flags = attrs.flags | CellFlag::WideCharContinuation };
    for (size_t i = 0; i < count && (col + i) < line.codepoints.size(); ++i)
    {
        line.codepoints[col + i] = 0;
        line.widths[col + i] = 1;
        line.clusterSize[col + i] = 0;
        line.sgr[col + i] = contAttrs;
        line.hyperlinks[col + i] = hyperlink;
    }

    // Continuation cells always have WideCharContinuation flag, which differs from the head cell's SGR.
    if (line.trivial && count > 0)
        line.trivial = false;
}

} // namespace vtbackend
