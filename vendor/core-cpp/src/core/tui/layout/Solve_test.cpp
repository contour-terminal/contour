// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Rect.hpp>
#include <core/tui/layout/Solve.hpp>
#include <core/tui/layout/Style.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

using core::tui::Rect;
using core::tui::Size;
using core::tui::layout::Align;
using core::tui::layout::Direction;
using core::tui::layout::Justify;
using core::tui::layout::Node;
using core::tui::layout::solve;
using core::tui::layout::Style;
using core::tui::layout::Wrap;

namespace
{

/// A leaf with @p style and a preferred size.
Node leaf(Style style, Size preferred = {})
{
    return Node { .style = style, .children = {}, .preferred = preferred };
}

/// A container styled @p style holding @p children.
Node container(Style style, std::vector<Node> children)
{
    return Node { .style = style, .children = std::move(children), .preferred = {} };
}

/// The rectangles of the root's direct children, which solve() lists after the root when no child has
/// children.
std::vector<Rect> childRects(Node const& root, Rect available)
{
    auto rects = solve(root, available);
    rects.erase(rects.begin());
    return rects;
}

/// The children's main sizes, along @p direction.
std::vector<int> mainSizes(std::span<Rect const> rects, Direction direction = Direction::Row)
{
    auto sizes = std::vector<int>();
    for (auto const& rect: rects)
        sizes.push_back(direction == Direction::Row ? rect.width : rect.height);
    return sizes;
}

std::vector<int> xs(std::span<Rect const> rects)
{
    auto positions = std::vector<int>();
    for (auto const& rect: rects)
        positions.push_back(rect.x);
    return positions;
}

Style growing(int grow)
{
    return Style { .grow = grow };
}

Style basisOf(int basis)
{
    return Style { .basis = basis };
}

} // namespace

TEST_CASE("layout.solve.growShares", "[tui][layout]")
{
    struct Row
    {
        char const* name;
        std::vector<int> grows;
        int width;
        int gap;
        std::vector<int> sizes;
        std::vector<int> positions;
    };
    // Leftover cells go to the earliest growing child, one each; gap is subtracted before sharing.
    auto const rows = std::vector<Row> {
        { .name = "equal grow, 10 over 3",
          .grows = { 1, 1, 1 },
          .width = 10,
          .gap = 0,
          .sizes = { 4, 3, 3 },
          .positions = { 0, 4, 7 } },
        { .name = "grow 1:2 over 10",
          .grows = { 1, 2 },
          .width = 10,
          .gap = 0,
          .sizes = { 4, 6 },
          .positions = { 0, 4 } },
        { .name = "gap first: 11 - 2x2 = 7 over 3",
          .grows = { 1, 1, 1 },
          .width = 11,
          .gap = 2,
          .sizes = { 3, 2, 2 },
          .positions = { 0, 5, 9 } },
        { .name = "a child with grow 0 keeps its basis",
          .grows = { 0, 1 },
          .width = 10,
          .gap = 0,
          .sizes = { 0, 10 },
          .positions = { 0, 0 } },
        { .name = "equal grow, 2 over 3",
          .grows = { 1, 1, 1 },
          .width = 2,
          .gap = 0,
          .sizes = { 1, 1, 0 },
          .positions = { 0, 1, 2 } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto children = std::vector<Node>();
        for (auto const grow: row.grows)
            children.push_back(leaf(growing(grow)));
        auto const rects = childRects(container(Style { .gap = row.gap }, std::move(children)),
                                      Rect { .x = 0, .y = 0, .width = row.width, .height = 2 });
        CHECK(mainSizes(rects) == row.sizes);
        CHECK(xs(rects) == row.positions);
        for (auto const& rect: rects)
            CHECK(rect.height == 2); // Align::Stretch is the default.
    }
}

TEST_CASE("layout.solve.shrinkShares", "[tui][layout]")
{
    struct Row
    {
        char const* name;
        std::vector<Style> styles;
        int width;
        std::vector<int> sizes;
    };
    auto const withMin = [](int basis, int minWidth) {
        return Style { .basis = basis, .minSize = { .width = minWidth, .height = 0 } };
    };
    auto const rows = std::vector<Row> {
        // A deficit of 3 over equal weights: one cell each, the remaining one from the last child.
        { .name = "ties to the last child",
          .styles = { basisOf(6), basisOf(6) },
          .width = 9,
          .sizes = { 5, 4 } },
        // Weights shrink x basis = 4 and 8: the deficit of 3 splits 1 and 2.
        { .name = "in proportion to shrink x basis",
          .styles = { basisOf(4), basisOf(8) },
          .width = 9,
          .sizes = { 3, 6 } },
        { .name = "shrink 0 keeps its basis",
          .styles = { Style { .shrink = 0, .basis = 6 }, basisOf(6) },
          .width = 9,
          .sizes = { 6, 3 } },
        // Pass 1 gives 4/3/3 and pushes the last child under its 5; frozen there, pass 2 shares the deficit
        // of 7 over the other two as 4/3, the odd cell taken from the latest of them. A single pass,
        // clamped and then clipped, would end at 4/3/3 instead.
        { .name = "a child at minSize is frozen and the rest re-shared",
          .styles = { basisOf(6), basisOf(6), withMin(6, 5) },
          .width = 10,
          .sizes = { 3, 2, 5 } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto children = std::vector<Node>();
        for (auto const& style: row.styles)
            children.push_back(leaf(style));
        auto const rects = childRects(container(Style {}, std::move(children)),
                                      Rect { .x = 0, .y = 0, .width = row.width, .height = 1 });
        CHECK(mainSizes(rects) == row.sizes);
    }
}

TEST_CASE("layout.solve.overflowClipsTheLastChild", "[tui][layout]")
{
    auto const atLeast4 = Style { .basis = 4, .minSize = { .width = 4, .height = 0 } };
    auto const rects = childRects(container(Style {}, { leaf(atLeast4), leaf(atLeast4) }),
                                  Rect { .x = 0, .y = 0, .width = 6, .height = 1 });
    CHECK(rects[0] == Rect { .x = 0, .y = 0, .width = 4, .height = 1 });
    CHECK(rects[1] == Rect { .x = 4, .y = 0, .width = 2, .height = 1 });
}

TEST_CASE("layout.solve.maxSizeReturnsCells", "[tui][layout]")
{
    // Pass 1 gives each 4; the first is clamped to 2 and the 10 left are shared again as 5/5.
    auto const capped = Style { .grow = 1, .maxSize = { .width = 2, .height = 0 } };
    auto const rects = childRects(container(Style {}, { leaf(capped), leaf(growing(1)), leaf(growing(1)) }),
                                  Rect { .x = 0, .y = 0, .width = 12, .height = 1 });
    CHECK(mainSizes(rects) == std::vector { 2, 5, 5 });
    CHECK(xs(rects) == std::vector { 0, 2, 7 });
}

TEST_CASE("layout.solve.justify", "[tui][layout]")
{
    struct Row
    {
        char const* name;
        Justify justify;
        int count;
        int width;
        std::vector<int> positions;
    };
    auto const rows = std::vector<Row> {
        { .name = "Start", .justify = Justify::Start, .count = 2, .width = 10, .positions = { 0, 2 } },
        { .name = "End", .justify = Justify::End, .count = 2, .width = 10, .positions = { 6, 8 } },
        { .name = "Center, the odd cell after",
          .justify = Justify::Center,
          .count = 1,
          .width = 9,
          .positions = { 3 } },
        { .name = "SpaceBetween",
          .justify = Justify::SpaceBetween,
          .count = 3,
          .width = 10,
          .positions = { 0, 4, 8 } },
        { .name = "SpaceBetween, the odd cell to the first gap",
          .justify = Justify::SpaceBetween,
          .count = 3,
          .width = 11,
          .positions = { 0, 5, 9 } },
        { .name = "SpaceBetween with one child is Start",
          .justify = Justify::SpaceBetween,
          .count = 1,
          .width = 10,
          .positions = { 0 } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto children = std::vector<Node>();
        for ([[maybe_unused]] auto const i: std::views::iota(0, row.count))
            children.push_back(leaf(basisOf(2)));
        auto const rects = childRects(container(Style { .justify = row.justify }, std::move(children)),
                                      Rect { .x = 0, .y = 0, .width = row.width, .height = 1 });
        CHECK(xs(rects) == row.positions);
        CHECK(mainSizes(rects) == std::vector<int>(static_cast<std::size_t>(row.count), 2));
    }
}

TEST_CASE("layout.solve.align", "[tui][layout]")
{
    struct Row
    {
        char const* name;
        Align align;
        Size maxSize;
        int y;
        int height;
    };
    auto const rows = std::vector<Row> {
        { .name = "Start", .align = Align::Start, .maxSize = {}, .y = 0, .height = 2 },
        { .name = "Center, the odd cell after", .align = Align::Center, .maxSize = {}, .y = 1, .height = 2 },
        { .name = "End", .align = Align::End, .maxSize = {}, .y = 3, .height = 2 },
        { .name = "Stretch", .align = Align::Stretch, .maxSize = {}, .y = 0, .height = 5 },
        { .name = "Stretch within maxSize",
          .align = Align::Stretch,
          .maxSize = { .width = 0, .height = 3 },
          .y = 0,
          .height = 3 },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const child = leaf(Style { .maxSize = row.maxSize }, Size { .width = 4, .height = 2 });
        auto const rects = childRects(container(Style { .align = row.align }, { child }),
                                      Rect { .x = 0, .y = 0, .width = 10, .height = 5 });
        CHECK(rects[0] == Rect { .x = 0, .y = row.y, .width = 4, .height = row.height });
    }
}

TEST_CASE("layout.solve.alignDefaultsToStretch", "[tui][layout]")
{
    STATIC_CHECK(Align {} == Align::Stretch);
    STATIC_CHECK(static_cast<Align>(0) == Align::Stretch);
    CHECK(Style {}.align == Align::Stretch);
}

TEST_CASE("layout.solve.wrapBreaksLines", "[tui][layout]")
{
    // 4 + 1 + 4 fits in 10, a third 4 does not. The lines' natural heights are 1 and 1; the one row of
    // leftover height goes to the first line, and the second starts after it and the gap.
    auto const child = leaf(basisOf(4), Size { .width = 0, .height = 1 });
    auto const rects = childRects(
        container(Style { .wrap = Wrap::Wrap, .align = Align::Start, .gap = 1 }, { child, child, child }),
        Rect { .x = 0, .y = 0, .width = 10, .height = 4 });
    CHECK(rects[0] == Rect { .x = 0, .y = 0, .width = 4, .height = 1 });
    CHECK(rects[1] == Rect { .x = 5, .y = 0, .width = 4, .height = 1 });
    CHECK(rects[2] == Rect { .x = 0, .y = 3, .width = 4, .height = 1 });

    auto const unwrapped = childRects(container(Style { .gap = 1 }, { child, child, child }),
                                      Rect { .x = 0, .y = 0, .width = 10, .height = 4 });
    CHECK(mainSizes(unwrapped) == std::vector { 3, 3, 2 }); // One line: the deficit of 4 is shrunk instead.
}

TEST_CASE("layout.solve.column", "[tui][layout]")
{
    auto const rects = childRects(container(Style { .direction = Direction::Column },
                                            { leaf(growing(1)), leaf(growing(1)), leaf(growing(1)) }),
                                  Rect { .x = 2, .y = 3, .width = 5, .height = 10 });
    CHECK(rects[0] == Rect { .x = 2, .y = 3, .width = 5, .height = 4 });
    CHECK(rects[1] == Rect { .x = 2, .y = 7, .width = 5, .height = 3 });
    CHECK(rects[2] == Rect { .x = 2, .y = 10, .width = 5, .height = 3 });
}

TEST_CASE("layout.solve.depthFirstOrder", "[tui][layout]")
{
    auto const inner = container(Style { .direction = Direction::Column, .grow = 1 },
                                 { leaf(growing(1)), leaf(growing(1)) });
    auto const root = container(Style {}, { inner, leaf(basisOf(3)) });
    auto const rects = solve(root, Rect { .x = 0, .y = 0, .width = 10, .height = 4 });
    REQUIRE(rects.size() == 5);
    CHECK(rects[0] == Rect { .x = 0, .y = 0, .width = 10, .height = 4 }); // the root
    CHECK(rects[1] == Rect { .x = 0, .y = 0, .width = 7, .height = 4 });  // inner
    CHECK(rects[2] == Rect { .x = 0, .y = 0, .width = 7, .height = 2 });  // inner's first
    CHECK(rects[3] == Rect { .x = 0, .y = 2, .width = 7, .height = 2 });  // inner's second
    CHECK(rects[4] == Rect { .x = 7, .y = 0, .width = 3, .height = 4 });  // the root's second
}

TEST_CASE("layout.solve.zeroWidth", "[tui][layout]")
{
    auto const wide = Style { .grow = 1, .basis = 4, .minSize = { .width = 2, .height = 1 } };
    auto const root =
        container(Style { .gap = 2 }, { leaf(wide), container(wide, { leaf(wide) }), leaf(wide) });
    for (auto const& available: { Rect { .x = 3, .y = 1, .width = 0, .height = 3 },
                                  Rect { .x = 3, .y = 1, .width = -5, .height = -1 } })
    {
        for (auto const& rect: solve(root, available))
        {
            CHECK(rect.width == 0);
            CHECK(rect.height >= 0);
            CHECK(rect.x == 3);
        }
    }
}

namespace
{

/// A deterministic generator (SplitMix64), so the property case draws the same trees everywhere.
class TreeDraw
{
  public:
    explicit TreeDraw(std::uint64_t seed): _state { seed } {}

    /// A number in [low, high].
    int between(int low, int high)
    {
        _state += 0x9E3779B97F4A7C15ULL;
        auto z = _state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        z ^= z >> 31U;
        return low + static_cast<int>(z % static_cast<std::uint64_t>(high - low + 1));
    }

    /// A number in [0, high], or now and then INT_MAX, which the layout must cap rather than overflow on.
    int cells(int high) { return between(0, 15) == 0 ? std::numeric_limits<int>::max() : between(0, high); }

    Size size(int high) { return Size { .width = cells(high), .height = cells(high) }; }

    Node node(int depth)
    {
        auto style = Style {
            .direction = static_cast<Direction>(between(0, 1)),
            .wrap = static_cast<Wrap>(between(0, 1)),
            .justify = static_cast<Justify>(between(0, 3)),
            .align = static_cast<Align>(between(0, 3)),
            .gap = between(-1, 3),
            .grow = between(-1, 3),
            .shrink = between(-1, 3),
            .basis = {},
            .minSize = size(6),
            .maxSize = size(14),
        };
        if (between(0, 1) == 1)
            style.basis = between(-2, 14);
        auto children = std::vector<Node>();
        if (depth > 0)
            for ([[maybe_unused]] auto const i: std::views::iota(0, between(0, 5)))
                children.push_back(node(depth - 1));
        return Node { .style = style, .children = std::move(children), .preferred = size(14) };
    }

  private:
    std::uint64_t _state;
};

std::size_t countNodes(Node const& node)
{
    auto count = std::size_t { 1 };
    for (auto const& child: node.children)
        count += countNodes(child);
    return count;
}

/// Checks @p node's subtree against @p rects from @p index on, and returns the index after it.
std::size_t checkSubtree(Node const& node, std::span<Rect const> rects, std::size_t index)
{
    auto const parent = rects[index];
    CHECK(parent.width >= 0);
    CHECK(parent.height >= 0);
    auto next = index + 1;
    auto siblings = std::vector<Rect>();
    for (auto const& child: node.children)
    {
        auto const rect = rects[next];
        CHECK(parent.contains(rect));
        for (auto const& sibling: siblings)
            CHECK(rect.intersect(sibling).empty());
        siblings.push_back(rect);
        next = checkSubtree(child, rects, next);
    }
    return next;
}

} // namespace

TEST_CASE("layout.solve.randomTreesStayInsideAndApart", "[tui][layout]")
{
    auto draw = TreeDraw { 0x5EEDU };
    for ([[maybe_unused]] auto const round: std::views::iota(0, 500))
    {
        auto const root = draw.node(3);
        auto const available =
            Rect { .x = draw.between(0, 5),
                   .y = draw.between(0, 5),
                   .width = draw.between(0, 15) == 0 ? draw.cells(40) : draw.between(-2, 40),
                   .height = draw.between(-2, 20) };
        auto const rects = solve(root, available);
        REQUIRE(rects.size() == countNodes(root));
        CHECK(checkSubtree(root, rects, 0) == rects.size());
        CHECK(solve(root, available) == rects); // deterministic
    }
}

TEST_CASE("layout.solve.hugeSizesStayInRange", "[tui][layout]")
{
    constexpr auto Huge = std::numeric_limits<int>::max();
    struct Row
    {
        char const* name;
        Style container;
        Style child;
    };
    auto const rows = std::vector<Row> {
        { .name = "shrink 1", .container = {}, .child = Style { .shrink = 1 } },
        { .name = "shrink 0", .container = {}, .child = Style { .shrink = 0 } },
        { .name = "shrink 1, wrapped",
          .container = Style { .wrap = Wrap::Wrap },
          .child = Style { .shrink = 1 } },
        { .name = "shrink 0, wrapped",
          .container = Style { .wrap = Wrap::Wrap },
          .child = Style { .shrink = 0 } },
        { .name = "huge gap, grow, shrink, basis and minSize",
          .container = Style { .wrap = Wrap::Wrap, .justify = Justify::SpaceBetween, .gap = Huge },
          .child = Style { .grow = Huge,
                           .shrink = Huge,
                           .basis = Huge,
                           .minSize = { .width = Huge, .height = Huge } } },
    };
    auto const areas = std::vector<Rect> {
        Rect { .x = 0, .y = 0, .width = 80, .height = 24 },
        Rect { .x = Huge, .y = Huge, .width = Huge, .height = Huge },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const child = leaf(row.child, Size { .width = Huge, .height = Huge });
        auto const root = container(row.container, { child, child, child });
        for (auto const& area: areas)
        {
            auto const rects = solve(root, area);
            REQUIRE(rects.size() == 4);
            CHECK(checkSubtree(root, rects, 0) == rects.size());
            CHECK(rects[0].right() <= core::tui::layout::MaxCells * 2);
        }
    }
}

TEST_CASE("layout.solve.sharesBeyond64BitProducts", "[tui][layout]")
{
    // 300 children of MaxCells over MaxCells: a deficit of 299 x 2^20 times a weight of 2^16 x 2^20 is past
    // 2^64, so the share is exact only if the product is not formed in 64 bits. Each gives up 299 x 2^20 /
    // 300 = 1045080 cells (and 224 odd cells from the last 224), leaving 3496 for the first 76 and 3495 for
    // the rest.
    using core::tui::layout::MaxCells;
    using core::tui::layout::MaxWeight;
    auto const child = leaf(Style { .shrink = MaxWeight, .basis = MaxCells });
    auto const root = container(Style {}, std::vector<Node>(300, child));
    auto const sizes = mainSizes(childRects(root, Rect { .x = 0, .y = 0, .width = MaxCells, .height = 1 }));
    REQUIRE(sizes.size() == 300);
    auto expected = std::vector<int>(76, 3496);
    expected.resize(300, 3495);
    CHECK(sizes == expected);
}

TEST_CASE("layout.solve.childrenPastMaxChildren", "[tui][layout]")
{
    // MaxChildren growing children share MaxCells (= MaxChildren) cells, one each; the two past the bound,
    // and the child beneath the last of them, are not laid out and sit zero-size at the parent's origin.
    using core::tui::layout::MaxCells;
    using core::tui::layout::MaxChildren;
    STATIC_REQUIRE(MaxChildren == static_cast<std::size_t>(MaxCells));
    auto children = std::vector<Node>(MaxChildren + 1, leaf(growing(1)));
    children.push_back(container(growing(1), { leaf(growing(1)) }));
    auto const root = container(Style {}, std::move(children));
    auto const rects = solve(root, Rect { .x = 3, .y = 4, .width = MaxCells, .height = 2 });
    REQUIRE(rects.size() == MaxChildren + 4);
    CHECK(rects[1] == Rect { .x = 3, .y = 4, .width = 1, .height = 2 });
    CHECK(rects[MaxChildren] == Rect { .x = 3 + MaxCells - 1, .y = 4, .width = 1, .height = 2 });
    auto const origin = Rect { .x = 3, .y = 4, .width = 0, .height = 0 };
    CHECK(rects[MaxChildren + 1] == origin);
    CHECK(rects[MaxChildren + 2] == origin);
    CHECK(rects[MaxChildren + 3] == origin);
}
