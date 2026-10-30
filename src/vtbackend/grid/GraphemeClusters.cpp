// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/GraphemeClusters.hpp>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>
#include <libunicode/width.h>

#include <algorithm>

namespace vtbackend
{

ColumnCount clusterColumns(std::u32string_view cluster, ClusterWidthPolicy policy) noexcept
{
    if (cluster.empty())
        return ColumnCount(0);
    auto const first = std::max(1u, unicode::width(cluster.front()));
    switch (policy)
    {
        case ClusterWidthPolicy::FirstCodepoint: return ColumnCount::cast_from(first);
        case ClusterWidthPolicy::ClusterAware:
            return ColumnCount::cast_from(std::max(first, unicode::grapheme_cluster_width(cluster)));
    }
    return ColumnCount::cast_from(first);
}

std::vector<MeasuredCluster> graphemeClustersOf(std::string_view text, ClusterWidthPolicy policy)
{
    auto clusters = std::vector<MeasuredCluster> {};
    for (auto const& cluster: unicode::utf8_grapheme_segmenter(text))
        if (!cluster.empty())
            clusters.push_back(
                MeasuredCluster { .text = unicode::convert_to<char>(std::u32string_view { cluster }),
                                  .columns = clusterColumns(cluster, policy) });
    return clusters;
}

ColumnCount columnsOnTheGrid(std::string_view text, ClusterWidthPolicy policy)
{
    auto columns = ColumnCount(0);
    for (auto const& cluster: unicode::utf8_grapheme_segmenter(text))
        columns += clusterColumns(cluster, policy);
    return columns;
}

} // namespace vtbackend
