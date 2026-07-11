// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace contour;
using contour::config::LayoutPane;

namespace
{
LayoutPane leaf(std::string cmd, double ratio = 1.0)
{
    LayoutPane p;
    p.command = std::move(cmd);
    p.ratio = ratio;
    return p;
}
LayoutPane split(vtmux::SplitState o, std::vector<LayoutPane> kids)
{
    LayoutPane p;
    p.orientation = o;
    p.children = std::move(kids);
    return p;
}
} // namespace

TEST_CASE("LayoutBuilder: leftmostLeaf descends first children", "[layout][builder]")
{
    auto tree = split(vtmux::SplitState::Vertical,
                      { split(vtmux::SplitState::Horizontal, { leaf("a"), leaf("b") }), leaf("c") });
    CHECK(*leftmostLeaf(tree).command == "a");
    CHECK(*leftmostLeaf(leaf("solo")).command == "solo");
}

TEST_CASE("LayoutBuilder: ratioForFirst splits weight of child0 vs the rest", "[layout][builder]")
{
    auto equalThree = split(vtmux::SplitState::Vertical, { leaf("a", 1.0), leaf("b", 1.0), leaf("c", 1.0) });
    CHECK(ratioForFirst(equalThree) == Catch::Approx(1.0 / 3.0));

    auto weighted = split(vtmux::SplitState::Vertical, { leaf("a", 0.6), leaf("b", 0.4) });
    CHECK(ratioForFirst(weighted) == Catch::Approx(0.6));
}

TEST_CASE("LayoutBuilder: tailGroup drops the first child", "[layout][builder]")
{
    auto three = split(vtmux::SplitState::Vertical, { leaf("a"), leaf("b"), leaf("c") });
    auto tail = tailGroup(three);
    REQUIRE_FALSE(tail.isLeaf());
    REQUIRE(tail.children.size() == 2);
    CHECK(*tail.children[0].command == "b");

    auto two = split(vtmux::SplitState::Vertical, { leaf("a"), leaf("b") });
    auto tail2 = tailGroup(two);
    CHECK(tail2.isLeaf()); // single remaining child collapses to that leaf
    CHECK(*tail2.command == "b");
}
