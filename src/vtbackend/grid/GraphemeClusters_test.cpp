// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/GraphemeClusters.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
/// One text, and the columns the grid gives it under either policy.
struct ClusterRow
{
    std::string_view name;
    std::string_view text;
    int clusterAware;   ///< Columns under ClusterWidthPolicy::ClusterAware (mode 2027 set).
    int firstCodepoint; ///< Columns under ClusterWidthPolicy::FirstCodepoint.
    std::size_t clusters;
};

constexpr auto ClusterRows = std::array {
    ClusterRow { .name = "ASCII", .text = "abc", .clusterAware = 3, .firstCodepoint = 3, .clusters = 3 },
    ClusterRow { .name = "CJK", .text = "漢字", .clusterAware = 4, .firstCodepoint = 4, .clusters = 2 },
    ClusterRow { .name = "decomposed é", .text = "é", .clusterAware = 1, .firstCodepoint = 1, .clusters = 1 },
    ClusterRow { .name = "ZWJ family",
                 .text = "\U0001F468‍\U0001F469‍\U0001F467",
                 .clusterAware = 2,
                 .firstCodepoint = 2,
                 .clusters = 1 },
    ClusterRow { .name = "flag",
                 .text = "\U0001F1E9\U0001F1EA",
                 .clusterAware = 2,
                 .firstCodepoint = 2,
                 .clusters = 1 },
    ClusterRow { .name = "VS16 heart", .text = "❤️", .clusterAware = 2, .firstCodepoint = 1, .clusters = 1 },
};
} // namespace

TEST_CASE("graphemeClusters.measuresAsTheGridLaysOut", "[grid][clusters]")
{
    for (auto const& row: ClusterRows)
    {
        INFO(row.name);
        CHECK(unbox<int>(columnsOnTheGrid(row.text, ClusterWidthPolicy::ClusterAware)) == row.clusterAware);
        CHECK(unbox<int>(columnsOnTheGrid(row.text, ClusterWidthPolicy::FirstCodepoint))
              == row.firstCodepoint);

        // Walked whole: never a cluster split, and the pieces add up to the text and to its width.
        auto const clusters = graphemeClustersOf(row.text, ClusterWidthPolicy::ClusterAware);
        CHECK(clusters.size() == row.clusters);
        auto joined = std::string {};
        auto columns = 0;
        for (auto const& cluster: clusters)
        {
            joined += cluster.text;
            columns += unbox<int>(cluster.columns);
        }
        CHECK(joined == row.text);
        CHECK(columns == row.clusterAware);
    }
}
