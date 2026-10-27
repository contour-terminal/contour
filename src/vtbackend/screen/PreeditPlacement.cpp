// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/PreeditPlacement.hpp>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>
#include <libunicode/width.h>

#include <algorithm>

namespace vtbackend
{

PreeditPlacement placePreedit(std::string_view preedit, ColumnOffset cursorColumn, ColumnCount lineColumns)
{
    auto clusters = std::vector<PreeditCluster> {};
    auto total = ColumnCount(0);
    for (std::u32string const& cluster: unicode::utf8_grapheme_segmenter(preedit))
    {
        // A cluster of no width -- a lone format character -- still takes a cell, as the grid's own text
        // does, so the preedit's cells and the columns they cover stay one to one.
        auto const width = ColumnCount::cast_from(std::max(1u, unicode::grapheme_cluster_width(cluster)));
        clusters.push_back(PreeditCluster { .text = cluster, .width = width });
        total += width;
    }

    auto const lineEnd = boxed_cast<ColumnOffset>(lineColumns);
    auto const start = cursorColumn + boxed_cast<ColumnOffset>(total) <= lineEnd
                           ? cursorColumn
                           : std::max(ColumnOffset(0), lineEnd - boxed_cast<ColumnOffset>(total));

    auto placement = PreeditPlacement { .start = start, .clusters = {}, .columns = ColumnCount(0) };
    auto const room = lineColumns - boxed_cast<ColumnCount>(start);
    for (auto& cluster: clusters)
    {
        if (placement.columns + cluster.width > room)
            break;
        placement.columns += cluster.width;
        placement.clusters.push_back(std::move(cluster));
    }
    return placement;
}

} // namespace vtbackend
