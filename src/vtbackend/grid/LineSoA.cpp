// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/LineSoA.hpp>

#include <libunicode/width.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iterator>
#include <limits>
#include <ranges>
#include <type_traits>

namespace vtbackend
{

namespace
{
    /// Calls @p visit once per per-cell array of a LineSoA, with the array's member and the value an
    /// empty cell holds in it. The one list of those arrays: every operation on all of them goes
    /// through here, so an array added to LineSoA cannot be left out of one.
    /// @param emptyAttrs The rendition an empty cell wears.
    /// @param visit      Called as `visit(&LineSoA::array, emptyValue)` for each array.
    template <typename Visitor>
    void forEachCellArray(GraphicsAttributes const& emptyAttrs, Visitor const& visit)
    {
        visit(&LineSoA::codepoints, char32_t { 0 });
        visit(&LineSoA::widths, uint8_t { 1 });
        visit(&LineSoA::scales, uint8_t { 1 });
        visit(&LineSoA::textScaleExtras, uint16_t { 0 });
        visit(&LineSoA::sgr, emptyAttrs);
        visit(&LineSoA::hyperlinks, HyperlinkId {});
        visit(&LineSoA::clusterSize, uint8_t { 0 });
        visit(&LineSoA::clusterPoolIndex, uint16_t { 0 });
    }

    /// The line-level state of a fresh line, blank or materialized, however its arrays were set.
    void resetLineMetadata(LineSoA& line, GraphicsAttributes const& fillAttrs) noexcept
    {
        line.imageFragments.reset();
        line.lineFlags = {};
        line.usedColumns = {};
        line.shape = LineShape::Uniform;
        line.fillAttrs = fillAttrs;
    }
} // namespace

void initializeLineSoA(LineSoA& line, ColumnCount cols, GraphicsAttributes const& fillAttrs)
{
    auto const n = unbox<size_t>(cols);
    forEachCellArray(fillAttrs, [&](auto array, auto const& empty) { (line.*array).assign(n, empty); });
    line.clusterPool.clear();
    resetLineMetadata(line, fillAttrs);
}

void initializeBlankLineSoA(LineSoA& line, GraphicsAttributes const& fillAttrs) noexcept
{
    // Swap with empty vectors to release capacity — clear() alone retains allocated memory
    // and would defeat the lazy-blank memory savings after a line cycles through
    // materialize → reset. For long sessions with ED/clear-screen ops, this keeps the
    // resident set proportional to currently-used lines rather than high-water mark.
    forEachCellArray(fillAttrs, [&](auto array, auto const& /*empty*/) {
        std::remove_cvref_t<decltype(line.*array)> {}.swap(line.*array);
    });
    std::vector<char32_t> {}.swap(line.clusterPool);
    resetLineMetadata(line, fillAttrs);
}

void recycleBlankLineSoA(LineSoA& line, GraphicsAttributes const& fillAttrs) noexcept
{
    // clear(), not a swap with an empty vector: size 0 is what makes the line blank (@see
    // isBlankLineSoA), and the capacity that stays is what the next materialization assigns into
    // without allocating.
    forEachCellArray(fillAttrs, [&](auto array, auto const& /*empty*/) { (line.*array).clear(); });
    line.clusterPool.clear();
    resetLineMetadata(line, fillAttrs);
}

void resizeLineSoA(LineSoA& line, ColumnCount newCols, GraphicsAttributes const& fillAttrs)
{
    // NOTE: resizeLineSoA does not preserve line.fillAttrs — callers operating on lazy-blank
    // Line objects must short-circuit via Line::resize() (which keeps blank lines blank) or
    // explicitly materialize first. Direct callers (e.g. reflow scratch buffers) start from
    // 0-column LineSoA where this is unambiguous.
    auto const oldSize = line.codepoints.size();
    auto const n = unbox<size_t>(newCols);
    forEachCellArray(fillAttrs, [&](auto array, auto const& empty) { (line.*array).resize(n, empty); });

    // Clamp usedColumns to new size
    if (line.usedColumns > newCols)
        line.usedColumns = newCols;

    // When growing, new cells get fillAttrs and no hyperlink. If existing cells wear a different SGR or
    // carry a hyperlink, the line mixes.
    if (n > oldSize && line.shape == LineShape::Uniform && oldSize > 0
        && (fillAttrs != line.sgr[0] || line.hyperlinks[0] != HyperlinkId {}))
        degradeShape(line, LineShape::Mixed);
}

void clearRange(LineSoA& line, size_t from, size_t count, GraphicsAttributes const& attrs)
{
    assert(!line.codepoints.empty() || count == 0);
    assert(from + count <= line.codepoints.size());

    forEachCellArray(attrs, [&](auto array, auto const& empty) {
        std::fill_n((line.*array).data() + from, count, empty);
    });

    // Remove image fragments in the cleared range
    if (line.imageFragments)
    {
        for (auto it = line.imageFragments->begin(); it != line.imageFragments->end();)
        {
            if (it->first >= from && it->first < from + count)
                it = line.imageFragments->erase(it);
            else
                ++it;
        }
    }

    // Partial clear may break SGR uniformity.
    if (from != 0 || count != line.codepoints.size())
        degradeShape(line, LineShape::Mixed);
}

void resetLine(LineSoA& line, ColumnCount cols, GraphicsAttributes const& fillAttrs)
{
    auto const n = unbox<size_t>(cols);

    // Callers using the lazy-blank model should call initializeBlankLineSoA instead;
    // resetLine still works via clearRange's empty-count fast path when the line is
    // blank and cols == 0.
    assert(!line.codepoints.empty() || n == 0);

    // A uniform line whose cells already wear fillAttrs and no hyperlink holds the right values in
    // every array but the ones a write changes. Compared against cell 0, not against the line's
    // fillAttrs: those only say what the line was last cleared with, and a full-width write keeps a
    // line uniform in another pen. Widths are cleared too, because a uniform line can still end in
    // one two-column cell (@see Line::trivialBuffer).
    auto const canSkipFill =
        line.shape == LineShape::Uniform
        && (n == 0 || (line.sgr[0] == fillAttrs && line.hyperlinks[0] == HyperlinkId {}));

    if (canSkipFill)
    {
        std::fill_n(line.codepoints.data(), n, char32_t { 0 });
        std::fill_n(line.widths.data(), n, uint8_t { 1 });
        std::fill_n(line.clusterSize.data(), n, uint8_t { 0 });
    }
    else
    {
        clearRange(line, 0, n, fillAttrs);
    }

    line.clusterPool.clear();
    line.imageFragments.reset();
    line.usedColumns = {};
    line.shape = LineShape::Uniform;
    line.fillAttrs = fillAttrs;
}

void copyColumns(LineSoA const& src, size_t srcCol, LineSoA& dst, size_t dstCol, size_t count)
{
    // Blank source: copying from an un-materialized line is semantically equivalent to
    // clearing the destination range with the source's fillAttrs. This lets callers
    // pass a blank line as the source without per-call-site guards.
    if (src.codepoints.empty())
    {
        if (count > 0)
            clearRange(dst, dstCol, count, src.fillAttrs);
        return;
    }
    assert(!dst.codepoints.empty() || count == 0);
    assert(srcCol + count <= src.codepoints.size());
    assert(dstCol + count <= dst.codepoints.size());

    // Bulk copy each SoA array (SIMD auto-vectorizable). The pool indices are the source line's, and
    // are rewritten for the destination's pool below.
    forEachCellArray(src.fillAttrs, [&](auto array, auto const& /*empty*/) {
        std::copy_n((src.*array).data() + srcCol, count, (dst.*array).data() + dstCol);
    });

    // Transfer cluster pool entries for cells with grapheme cluster extras
    for (size_t i = 0; i < count; ++i)
    {
        if (src.clusterSize[srcCol + i] > 1)
        {
            auto const poolStart = src.clusterPoolIndex[srcCol + i];
            auto const extraCount = static_cast<size_t>(src.clusterSize[srcCol + i] - 1);

            dst.clusterPoolIndex[dstCol + i] = static_cast<uint16_t>(dst.clusterPool.size());
            auto const poolStartIt = src.clusterPool.begin() + static_cast<ptrdiff_t>(poolStart);
            dst.clusterPool.insert(
                dst.clusterPool.end(), poolStartIt, poolStartIt + static_cast<ptrdiff_t>(extraCount));
        }
        else
        {
            dst.clusterPoolIndex[dstCol + i] = 0;
        }
    }

    // Transfer image fragments in the copied range
    if (src.imageFragments)
    {
        for (auto const& [col, frag]: *src.imageFragments)
        {
            if (col >= srcCol && col < srcCol + count)
            {
                if (!dst.imageFragments)
                    dst.imageFragments.emplace();
                (*dst.imageFragments)[static_cast<uint16_t>(dstCol + (col - srcCol))] = frag;
            }
        }
    }

    // Copied cells may break the destination's uniformity, and they bring their own line's spans.
    if (count > 0)
        degradeShape(dst, std::max(LineShape::Mixed, src.shape));
}

void moveColumns(LineSoA& line, size_t srcCol, size_t dstCol, size_t count)
{
    assert(!line.codepoints.empty() || count == 0);
    assert(srcCol + count <= line.codepoints.size());
    assert(dstCol + count <= line.codepoints.size());

    // Use memmove for overlapping ranges (one per array)
    forEachCellArray(line.fillAttrs, [&](auto array, auto const& /*empty*/) {
        auto* const data = (line.*array).data();
        std::memmove(data + dstCol, data + srcCol, count * sizeof(*data));
    });

    // Note: moveColumns does NOT handle cluster pool or image fragment reindexing.
    // Callers must handle cluster pool entries if source cells have extras.
    // For insert/delete operations, the cluster pool entries remain valid
    // because the pool indices move with the clusterPoolIndex array.
}

size_t trimBlankRight(LineSoA const& line, size_t cols)
{
    // Blank lines have no codepoints to scan; the line is uniformly empty by definition.
    if (line.codepoints.empty())
        return 0;
    auto end = cols;
    while (end > 0 && line.codepoints[end - 1] == 0)
        --end;
    return end;
}

namespace
{
    /// Highest pool size @c LineSoA::clusterPoolIndex (a uint16_t) can still address.
    constexpr size_t ClusterPoolLimit = std::numeric_limits<uint16_t>::max();

    /// Rebuilds @c clusterPool from the cells that actually reference it, dropping the runs abandoned
    /// by earlier overwrites, and places @c tailCol's run last.
    ///
    /// The pool is append-only: @c clearClusterExtras deliberately does not compact, so every rewrite
    /// of a cell carrying extra codepoints appends a fresh run and orphans the old one. A long-lived
    /// line rewritten in place -- a status line, a spinner cycling emoji -- therefore grows the pool
    /// without bound until @c clusterPoolIndex can no longer address it and silently wraps, at which
    /// point cells read back an unrelated codepoint run.
    ///
    /// @param tailCol the column whose run must end up at the tail, because the caller is about to
    ///                extend it with @c push_back.
    void compactClusterPool(LineSoA& line, size_t tailCol)
    {
        auto compacted = std::vector<char32_t> {};
        compacted.reserve(line.clusterPool.size());

        auto const copyRun = [&](size_t col) {
            auto const start = static_cast<size_t>(line.clusterPoolIndex[col]);
            auto const extraCount = static_cast<size_t>(line.clusterSize[col] - 1);
            line.clusterPoolIndex[col] = static_cast<uint16_t>(compacted.size());
            compacted.insert(compacted.end(),
                             std::next(line.clusterPool.begin(), static_cast<ptrdiff_t>(start)),
                             std::next(line.clusterPool.begin(), static_cast<ptrdiff_t>(start + extraCount)));
        };

        for (auto const col: std::views::iota(size_t { 0 }, line.clusterSize.size()))
            if (col != tailCol && line.clusterSize[col] > 1)
                copyRun(col);

        // Appending to a cluster assumes its run is at the tail of the pool, so tailCol goes last.
        if (line.clusterSize[tailCol] > 1)
            copyRun(tailCol);

        line.clusterPool = std::move(compacted);
    }
} // namespace

int appendCodepointToCluster(LineSoA& line, size_t col, char32_t codepoint, ClusterWidthPolicy policy)
{
    assert(!line.codepoints.empty());
    auto const currentSize = line.clusterSize[col];

    auto const oldWidth = static_cast<unsigned>(line.widths[col]);

    if (currentSize < MaxGraphemeClusterSize)
    {
        // A run must start at an offset a uint16_t can address. Compacting first reclaims the runs
        // abandoned by earlier overwrites; only if the LIVE extras genuinely fill the pool is the
        // codepoint dropped, and no page width Contour supports can reach that.
        if (line.clusterPool.size() + MaxGraphemeClusterSize > ClusterPoolLimit)
            compactClusterPool(line, col);

        if (line.clusterPool.size() < ClusterPoolLimit)
        {
            if (currentSize == 1)
            {
                // First extra codepoint — record start index in pool
                line.clusterPoolIndex[col] = static_cast<uint16_t>(line.clusterPool.size());
            }

            line.clusterPool.push_back(codepoint);
            line.clusterSize[col] = currentSize + 1;

            // The batched RenderLine path stores ONE codepoint per column (@see
            // Line::trivialBuffer), so a cell holding a cluster cannot be expressed there at all:
            // everything after the base is silently dropped, and a decomposed "e" + U+0301 draws
            // without its accent. The line becomes Mixed, which leaves that path, just as a wide
            // cell's continuation (@see fillWideCharContinuation) and an image fragment (@see
            // CellProxy::setImageFragment) make it Spanning.
            //
            // Independent of @p policy and of any width change: under FirstCodepoint the cluster
            // keeps its width, but the codepoint still joins it -- and it is the codepoint, not the
            // width, that the batched representation cannot carry.
            //
            // Only when a cluster is actually FORMED. Appending to a cell with no base codepoint
            // (currentSize == 0) leaves the pool entry unreferenced -- clusterPoolIndex is set only
            // on the transition from 1 -- so the cell still reads back as its single (empty) base
            // and stays perfectly expressible. Degrading the line there would cost it its fast path
            // for a codepoint nothing can read.
            //
            // Mixed, not Spanning: the cell still occupies exactly its own column, so nothing about
            // it stops a later ASCII run from being bulk-written over it.
            if (currentSize >= 1)
                degradeShape(line, LineShape::Mixed);
        }
    }

    // The cluster's width is recomputed over the WHOLE cluster rather than adjusted incrementally:
    // the rules need lookbehind (a consonant widens its cluster only when a virama precedes it, and
    // a ZWJ swallows whatever follows), which a per-codepoint delta cannot express.
    //
    // Copying into a stack buffer keeps this allocation-free -- a cluster is bounded by
    // MaxGraphemeClusterSize -- and it only runs for continuation codepoints, never for ASCII.
    std::array<char32_t, MaxGraphemeClusterSize> buffer {};
    size_t count = 0;
    forEachCodepoint(line, col, [&](char32_t cp) {
        if (count < buffer.size())
            buffer[count++] = cp;
    });
    // Under FirstCodepoint the codepoint still joins the cluster -- it is part of the text, and must
    // round-trip through a copy -- but it may not change how many columns the cluster occupies.
    if (policy == ClusterWidthPolicy::FirstCodepoint)
        return 0;

    // A cluster always occupies at least one column. grapheme_cluster_width answers 0 for a cluster
    // made only of zero-width codepoints -- a lone combining mark, ZWSP, a bare ZWJ -- and the base
    // codepoint was stored under exactly this clamp by writeNonAsciiToSoA. Without it here the width
    // both violates the "1 or 2 columns" invariant LineSoA.h documents and drives the shrink path's
    // erase range onto the head cell itself, wiping the text the revision was meant to keep.
    auto const newWidth =
        std::max(1u, unicode::grapheme_cluster_width(std::u32string_view(buffer.data(), count)));
    line.widths[col] = static_cast<uint8_t>(newWidth);

    return static_cast<int>(newWidth) - static_cast<int>(oldWidth);
}

void clearClusterExtras(LineSoA& line, size_t col)
{
    assert(!line.codepoints.empty());
    // Mark the cell as having no extras.
    // The old pool entries become garbage — they'll be cleaned on line reset (lazy compaction).
    if (line.clusterSize[col] > 1)
    {
        line.clusterSize[col] = std::min(line.clusterSize[col], uint8_t { 1 });
        line.clusterPoolIndex[col] = 0;
    }
}

LineShape shapeImpliedByCells(LineSoA const& line) noexcept
{
    auto shape = LineShape::Uniform;
    for (auto const col: std::views::iota(size_t { 0 }, line.codepoints.size()))
    {
        auto const hasImage =
            line.imageFragments && line.imageFragments->contains(static_cast<uint16_t>(col));
        if (hasImage || isSpanningCell(line.sgr[col].flags, line.scales[col]))
            return LineShape::Spanning;
        if (line.sgr[col] != line.sgr[0] || line.hyperlinks[col] != line.hyperlinks[0]
            || line.clusterSize[col] > 1 || line.textScaleExtras[col] != 0)
            shape = LineShape::Mixed;
    }
    return shape;
}

} // namespace vtbackend
