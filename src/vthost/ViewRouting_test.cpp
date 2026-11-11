// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vthost/ViewRouting.hpp>

using namespace vthost;

namespace
{

using Views = std::vector<ConnectionView>;
using Ids = std::vector<uint64_t>;

ConnectionId connection(uint64_t index)
{
    return ConnectionId { .endpoint = "native", .index = index, .peer = {} };
}

ConnectionView view(uint64_t index, uint64_t selected, uint64_t focusSerial)
{
    return ConnectionView { .connection = connection(index),
                            .selected = selected,
                            .focusSerial = focusSerial };
}

std::optional<ConnectionId> some(uint64_t index)
{
    return connection(index);
}

} // namespace

TEST_CASE("raiseHolder picks the most recently focused window", "[vthost][routing]")
{
    struct Row
    {
        Views views;
        std::optional<ConnectionId> expected;
    };
    auto const rows = std::vector<Row> {
        { {}, std::nullopt },
        { { view(1, 10, 0) }, some(1) },
        { { view(1, 10, 3), view(2, 10, 7) }, some(2) },
        { { view(1, 10, 7), view(2, 10, 3) }, some(1) },
        { { view(1, 10, 0), view(2, 10, 0), view(3, 10, 0) }, some(3) },
        { { view(1, 10, 3), view(2, 10, 0) }, some(1) },
    };
    for (auto const& row: rows)
        CHECK(raiseHolder(row.views) == row.expected);
}

TEST_CASE("clipboardTarget picks among the windows selecting the pane's workspace", "[vthost][routing]")
{
    struct Row
    {
        Views views;
        uint64_t workspace;
        std::optional<ConnectionId> expected;
    };
    auto const rows = std::vector<Row> {
        { {}, 10, std::nullopt },
        { { view(1, 10, 0) }, 10, some(1) },
        { { view(1, 10, 3), view(2, 11, 7) }, 12, std::nullopt },
        { { view(1, 10, 3), view(2, 11, 7) }, 10, some(1) },
        { { view(1, 10, 3), view(2, 10, 7), view(3, 11, 9) }, 10, some(2) },
        { { view(1, 10, 0), view(2, 10, 0) }, 10, some(2) },
    };
    for (auto const& row: rows)
        CHECK(clipboardTarget(row.views, row.workspace) == row.expected);
}

TEST_CASE("switchTarget follows the workspace when named and the raise holder otherwise", "[vthost][routing]")
{
    struct Row
    {
        Views views;
        std::optional<uint64_t> workspace;
        std::optional<ConnectionId> expected;
    };
    auto const rows = std::vector<Row> {
        { {}, std::nullopt, std::nullopt },
        { {}, 10, std::nullopt },
        { { view(1, 10, 3), view(2, 11, 7) }, std::nullopt, some(2) },
        { { view(1, 10, 3), view(2, 11, 7) }, 10, some(1) },
        { { view(1, 10, 3), view(2, 11, 7) }, 12, std::nullopt },
        { { view(1, 10, 0), view(2, 10, 0) }, 10, some(2) },
    };
    for (auto const& row: rows)
        CHECK(switchTarget(row.views, row.workspace) == row.expected);
}

TEST_CASE("defaultSelection keeps the last selection while it exists", "[vthost][routing]")
{
    struct Row
    {
        Ids workspaces;
        std::optional<uint64_t> last;
        std::optional<uint64_t> expected;
    };
    auto const rows = std::vector<Row> {
        { {}, std::nullopt, std::nullopt },
        { {}, 5, std::nullopt },
        { { 4, 5, 6 }, 5, 5 },
        { { 4, 5, 6 }, 9, 4 },
        { { 4, 5, 6 }, std::nullopt, 4 },
    };
    for (auto const& row: rows)
        CHECK(defaultSelection(row.workspaces, row.last) == row.expected);
}
