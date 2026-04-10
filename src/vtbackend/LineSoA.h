// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/LineFlags.h>
#include <vtbackend/primitives.h>

#include <crispy/AlignedAllocator.h>

#include <cstdint>
#include <optional>

namespace vtbackend
{

/// Convenience alias: 64-byte aligned vector for SIMD-friendly SoA arrays.
template <typename T>
using AlignedVector = crispy::aligned_vector<T>;

/// Maximum number of codepoints in a grapheme cluster (matches CompactCell::MaxCodepoints).
inline constexpr uint8_t MaxGraphemeClusterSize = 7;

/// Structure-of-Arrays storage for one terminal line.
///
/// Each array has exactly `columns` elements. All arrays are always eagerly populated
/// (no lazy inflation). This enables uniform access patterns and SIMD-friendly bulk operations.
///
/// Arrays are grouped into tiers by access frequency:
/// - Tier 1 (Hot): codepoints, widths — touched on every character write
/// - Tier 2 (Warm): colors, flags — touched when SGR is non-default
/// - Tier 3 (Cold): hyperlinks — rarely non-default
///
/// Grapheme clusters with multiple codepoints store the primary codepoint in @c codepoints[]
/// and extra codepoints in a flat @c clusterPool, indexed by @c clusterPoolIndex[].
struct LineSoA
{
    // --- Tier 1: Hot — touched on every character write and render ---

    /// Primary Unicode codepoint per cell (first codepoint of the grapheme cluster).
    AlignedVector<char32_t> codepoints;

    /// East Asian Width display width per cell (1 or 2 columns).
    AlignedVector<uint8_t> widths;

    // --- Tier 2: Warm — SGR "pen" attributes, always read/written together ---

    /// Graphics attributes per cell (foreground, background, underline color + flags).
    /// Packed as a single struct for cache locality — one write per cell instead of four.
    AlignedVector<GraphicsAttributes> sgr;

    // --- Tier 3: Cold — rarely non-default ---

    /// Hyperlink ID per cell.
    AlignedVector<HyperlinkId> hyperlinks;

    // --- Grapheme cluster overflow ---

    /// Number of codepoints in the grapheme cluster at each cell.
    /// 0 = empty cell, 1 = single codepoint (primary only), 2+ = has extras in clusterPool.
    AlignedVector<uint8_t> clusterSize;

    /// Index into @c clusterPool where this cell's extra codepoints start.
    /// Only meaningful when @c clusterSize[col] >= 2.
    AlignedVector<uint16_t> clusterPoolIndex;

    /// Flat pool of extra codepoints for all multi-codepoint grapheme clusters on this line.
    /// Cleared on line reset (lazy compaction).
    std::vector<char32_t> clusterPool;

    // --- Image fragments (extremely rare, null 99.99% of lines) ---

    /// Column → image fragment mapping. Disengaged when no images on this line.
    /// Engaged on first setImageFragment(), reset on resetLine().
    using ImageFragmentMap = std::unordered_map<uint16_t, std::shared_ptr<ImageFragment>>;
    std::optional<ImageFragmentMap> imageFragments;

    // --- Line-level metadata ---

    /// Line flags (Wrappable, Wrapped, Marked, etc.).
    LineFlags lineFlags {};

    /// Number of columns populated with content (for efficient trim operations).
    ColumnCount usedColumns {};

    /// Cached: true when all written cells share uniform SGR attributes.
    /// Set to true on line init/reset, set to false when a cell is written
    /// with different SGR than the first cell. Read as O(1) by the render path.
    bool trivial = true;

    /// The GraphicsAttributes used in the most recent full-line reset/clear.
    /// Used by writeAsciiToSoA and resetLine to skip redundant fill operations
    /// when the line is being rewritten with the same default attributes.
    GraphicsAttributes fillAttrs {};
};

// ---------------------------------------------------------------------------
// LineSoA helper functions
// ---------------------------------------------------------------------------

/// Initialize all arrays to the given column count with default values.
/// @param line      The LineSoA to initialize.
/// @param cols      Number of columns.
/// @param fillAttrs Default graphics attributes for empty cells.
void initializeLineSoA(LineSoA& line, ColumnCount cols, GraphicsAttributes const& fillAttrs = {});

/// Resize all arrays to a new column count, preserving existing data.
/// New columns (if growing) are initialized with @p fillAttrs.
void resizeLineSoA(LineSoA& line, ColumnCount newCols, GraphicsAttributes const& fillAttrs = {});

/// Clear a column range to empty cells with the given attributes.
/// Each array is filled independently via std::fill_n (SIMD auto-vectorizable).
void clearRange(LineSoA& line, size_t from, size_t count, GraphicsAttributes const& attrs);

/// Reset an entire line to empty state.
void resetLine(LineSoA& line, ColumnCount cols, GraphicsAttributes const& fillAttrs = {});

/// Copy a column range from one line to another (bulk memcpy per array).
/// Handles cluster pool transfer and image fragments.
void copyColumns(LineSoA const& src, size_t srcCol, LineSoA& dst, size_t dstCol, size_t count);

/// Move columns within a line (for insert/delete character operations).
/// Uses memmove per array to handle overlapping ranges.
void moveColumns(LineSoA& line, size_t srcCol, size_t dstCol, size_t count);

/// Find the last non-empty column (scan codepoints[] for last non-zero).
/// @return Number of used columns (0 if line is empty).
[[nodiscard]] size_t trimBlankRight(LineSoA const& line, size_t cols);

/// Iterate over all codepoints of a grapheme cluster at the given column.
/// @param line The LineSoA to read from.
/// @param col  Column index.
/// @param f    Callback invoked with each char32_t codepoint.
template <typename F>
void forEachCodepoint(LineSoA const& line, size_t col, F f)
{
    if (line.clusterSize[col] == 0)
        return;

    f(line.codepoints[col]);

    if (line.clusterSize[col] > 1)
    {
        auto const start = line.clusterPoolIndex[col];
        auto const extraCount = static_cast<size_t>(line.clusterSize[col] - 1);
        for (size_t i = 0; i < extraCount; ++i)
            f(line.clusterPool[start + i]);
    }
}

/// Append a continuation codepoint to an existing grapheme cluster.
/// @return Width change (0 in current implementation unless AllowWidthChange is enabled).
int appendCodepointToCluster(LineSoA& line, size_t col, char32_t codepoint);

/// Clear cluster overflow data for a specific column.
/// Note: this does not compact the pool; garbage entries remain until line reset.
void clearClusterExtras(LineSoA& line, size_t col);

} // namespace vtbackend
