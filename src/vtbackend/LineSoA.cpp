// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/LineSoA.h>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace vtbackend
{

void initializeLineSoA(LineSoA& line, ColumnCount cols, GraphicsAttributes const& fillAttrs)
{
    auto const n = unbox<size_t>(cols);

    line.codepoints.assign(n, char32_t { 0 });
    line.widths.assign(n, uint8_t { 1 });
    line.sgr.assign(n, fillAttrs);
    line.hyperlinks.assign(n, HyperlinkId {});
    line.clusterSize.assign(n, uint8_t { 0 });
    line.clusterPoolIndex.assign(n, uint16_t { 0 });

    line.clusterPool.clear();
    line.imageFragments.reset();
    line.lineFlags = {};
    line.usedColumns = {};
    line.trivial = true;
    line.fillAttrs = fillAttrs;
}

void resizeLineSoA(LineSoA& line, ColumnCount newCols, GraphicsAttributes const& fillAttrs)
{
    auto const n = unbox<size_t>(newCols);

    line.codepoints.resize(n, char32_t { 0 });
    line.widths.resize(n, uint8_t { 1 });
    line.sgr.resize(n, fillAttrs);
    line.hyperlinks.resize(n, HyperlinkId {});
    line.clusterSize.resize(n, uint8_t { 0 });
    line.clusterPoolIndex.resize(n, uint16_t { 0 });

    // Clamp usedColumns to new size
    if (line.usedColumns > newCols)
        line.usedColumns = newCols;
}

void clearRange(LineSoA& line, size_t from, size_t count, GraphicsAttributes const& attrs)
{
    assert(from + count <= line.codepoints.size());

    std::fill_n(line.codepoints.data() + from, count, char32_t { 0 });
    std::fill_n(line.widths.data() + from, count, uint8_t { 1 });
    std::fill_n(line.sgr.data() + from, count, attrs);
    std::fill_n(line.hyperlinks.data() + from, count, HyperlinkId {});
    std::fill_n(line.clusterSize.data() + from, count, uint8_t { 0 });
    std::fill_n(line.clusterPoolIndex.data() + from, count, uint16_t { 0 });

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
        line.trivial = false;
}

void resetLine(LineSoA& line, ColumnCount cols, GraphicsAttributes const& fillAttrs)
{
    auto const n = unbox<size_t>(cols);

    // When the line was used in a "clean" manner (uniform SGR, same fill attrs,
    // no hyperlinks), the widths/sgr/hyperlinks/clusterPoolIndex arrays already
    // contain correct default values. Only codepoints and clusterSize need clearing
    // (their values change between write and reset: chars→0, 1→0).
    auto const canSkipFill =
        line.trivial && (fillAttrs == line.fillAttrs) && (n == 0 || line.hyperlinks[0] == HyperlinkId {});

    if (canSkipFill)
    {
        std::fill_n(line.codepoints.data(), n, char32_t { 0 });
        std::fill_n(line.clusterSize.data(), n, uint8_t { 0 });
    }
    else
    {
        clearRange(line, 0, n, fillAttrs);
    }

    line.clusterPool.clear();
    line.imageFragments.reset();
    line.usedColumns = {};
    line.trivial = true;
    line.fillAttrs = fillAttrs;
}

void copyColumns(LineSoA const& src, size_t srcCol, LineSoA& dst, size_t dstCol, size_t count)
{
    assert(srcCol + count <= src.codepoints.size());
    assert(dstCol + count <= dst.codepoints.size());

    // Bulk copy each SoA array (SIMD auto-vectorizable)
    std::copy_n(src.codepoints.data() + srcCol, count, dst.codepoints.data() + dstCol);
    std::copy_n(src.widths.data() + srcCol, count, dst.widths.data() + dstCol);
    std::copy_n(src.sgr.data() + srcCol, count, dst.sgr.data() + dstCol);
    std::copy_n(src.hyperlinks.data() + srcCol, count, dst.hyperlinks.data() + dstCol);
    std::copy_n(src.clusterSize.data() + srcCol, count, dst.clusterSize.data() + dstCol);

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
}

void moveColumns(LineSoA& line, size_t srcCol, size_t dstCol, size_t count)
{
    assert(srcCol + count <= line.codepoints.size());
    assert(dstCol + count <= line.codepoints.size());

    // Use memmove for overlapping ranges (one per array)
    std::memmove(line.codepoints.data() + dstCol, line.codepoints.data() + srcCol, count * sizeof(char32_t));
    std::memmove(line.widths.data() + dstCol, line.widths.data() + srcCol, count * sizeof(uint8_t));
    std::memmove(line.sgr.data() + dstCol, line.sgr.data() + srcCol, count * sizeof(GraphicsAttributes));
    std::memmove(
        line.hyperlinks.data() + dstCol, line.hyperlinks.data() + srcCol, count * sizeof(HyperlinkId));
    std::memmove(line.clusterSize.data() + dstCol, line.clusterSize.data() + srcCol, count * sizeof(uint8_t));
    std::memmove(line.clusterPoolIndex.data() + dstCol,
                 line.clusterPoolIndex.data() + srcCol,
                 count * sizeof(uint16_t));

    // Note: moveColumns does NOT handle cluster pool or image fragment reindexing.
    // Callers must handle cluster pool entries if source cells have extras.
    // For insert/delete operations, the cluster pool entries remain valid
    // because the pool indices move with the clusterPoolIndex array.
}

size_t trimBlankRight(LineSoA const& line, size_t cols)
{
    auto end = cols;
    while (end > 0 && line.codepoints[end - 1] == 0)
        --end;
    return end;
}

int appendCodepointToCluster(LineSoA& line, size_t col, char32_t codepoint)
{
    auto const currentSize = line.clusterSize[col];
    if (currentSize >= MaxGraphemeClusterSize)
        return 0;

    if (currentSize == 1)
    {
        // First extra codepoint — record start index in pool
        line.clusterPoolIndex[col] = static_cast<uint16_t>(line.clusterPool.size());
    }

    line.clusterPool.push_back(codepoint);
    line.clusterSize[col] = currentSize + 1;

    // Width change computation (currently always returns 0 unless AllowWidthChange is enabled)
    // Mirrors CellUtil::computeWidthChange behavior
    return 0;
}

void clearClusterExtras(LineSoA& line, size_t col)
{
    // Mark the cell as having no extras.
    // The old pool entries become garbage — they'll be cleaned on line reset (lazy compaction).
    if (line.clusterSize[col] > 1)
    {
        line.clusterSize[col] = std::min(line.clusterSize[col], uint8_t { 1 });
        line.clusterPoolIndex[col] = 0;
    }
}

} // namespace vtbackend
