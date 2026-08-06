// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PageSize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>

#include <vthost/tmux/LayoutString.hpp>
#include <vtworkspace/Pane.hpp>

using vthost::tmux::BinaryLayout;
using vthost::tmux::collapseToBinary;
using vthost::tmux::encodeLayout;
using vthost::tmux::layoutChecksum;
using vthost::tmux::ParsedLayout;
using vthost::tmux::parseLayout;
using vtpty::ColumnCount;
using vtpty::LineCount;
using vtpty::PageSize;
using vtworkspace::Pane;
using vtworkspace::PaneId;
using vtworkspace::SessionId;
using vtworkspace::SplitState;

namespace
{

/// Builds `left | (top / bottom)` — three leaves, two orientations.
struct SampleTree
{
    Pane root { PaneId { 1 }, SessionId { 100 } };
    Pane* left = nullptr;
    Pane* top = nullptr;
    Pane* bottom = nullptr;

    SampleTree()
    {
        auto const [first, second] =
            root.split(SplitState::Vertical, PaneId { 2 }, PaneId { 3 }, SessionId { 101 }, 0.5);
        left = first;
        auto const [t, b] =
            second->split(SplitState::Horizontal, PaneId { 4 }, PaneId { 5 }, SessionId { 102 }, 0.3);
        top = t;
        bottom = b;
    }
};

} // namespace

TEST_CASE("layout checksum matches tmux's rotate-add algorithm", "[vthost][layout]")
{
    // Manually computed over "ab": 'a' (97), then rotate(97)=48+... — assert the
    // algebra rather than a magic value: rotate-right of odd sets the top bit.
    CHECK(layoutChecksum("") == 0);
    CHECK(layoutChecksum("a") == 97);
    CHECK(layoutChecksum("b") == 98);
    // rotate(97) = (97>>1) | (1<<15) = 48 + 32768; + 'b'(98) = 32914
    CHECK(layoutChecksum("ab") == 32914);
}

TEST_CASE("encodeLayout emits geometry the parser round-trips as a tree", "[vthost][layout]")
{
    auto tree = SampleTree {};
    auto const encoded = encodeLayout(tree.root, PageSize { LineCount(50), ColumnCount(160) });

    // 159 divisible columns: 80 | 78; right column 78x50 splits into 15 / 34 lines.
    CHECK(encoded.contains("160x50,0,0"));

    auto const parsed = parseLayout(encoded);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind == ParsedLayout::Kind::SideBySide);
    REQUIRE(parsed->children.size() == 2);
    // Pane::split keeps the ORIGINAL id on the first child and gives the split
    // node the new one, so the leaves here are 1 (left), 3 (top), 5 (bottom).
    CHECK(parsed->children[0].paneId == 1);
    CHECK(parsed->children[0].width + 1 + parsed->children[1].width == 160);
    REQUIRE(parsed->children[1].kind == ParsedLayout::Kind::Stacked);
    CHECK(parsed->children[1].children[0].paneId == 3);
    CHECK(parsed->children[1].children[1].paneId == 5);
}

TEST_CASE("a corrupted checksum is rejected", "[vthost][layout]")
{
    auto tree = SampleTree {};
    auto encoded = encodeLayout(tree.root, PageSize { LineCount(50), ColumnCount(160) });
    encoded[0] = encoded[0] == '0' ? '1' : '0';
    CHECK_FALSE(parseLayout(encoded).has_value());
}

TEST_CASE("a layout violating the partition arithmetic is rejected", "[vthost][layout]")
{
    // 80 + 1 + 80 != 160: layout_check must refuse, exactly like tmux.
    auto const body = std::string { "160x50,0,0{80x50,0,0,1,80x50,81,0,2}" };
    auto const wire = std::format("{:04x},{}", layoutChecksum(body), body);
    auto const parsed = parseLayout(wire);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().contains("partition"));
}

TEST_CASE("a pathologically nested layout is refused, not crashed", "[vthost][layout]")
{
    // A deep '{' spine would recurse the parser (and the check/collapse passes) once per level;
    // unbounded, that overflows the call stack. The depth guard turns it into a clean error instead.
    // The checksum guarding the body is forgeable, so a hostile control server can send exactly this.
    auto body = std::string {};
    for ([[maybe_unused]] auto const level: std::views::iota(0, 4000))
        body += "1x1,0,0{";
    auto const wire = std::format("{:04x},{}", layoutChecksum(body), body);
    auto const parsed = parseLayout(wire);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().contains("deep"));
}

TEST_CASE("layout dimensions near INT_MAX do not overflow the partition check", "[vthost][layout]")
{
    // Child extents parse into int and are summed along the split axis. In a 32-bit accumulator two
    // INT_MAX-wide children overflow (signed-overflow UB — a UBSan abort on dev/CI builds); the sum
    // must be computed wide so the check rejects cleanly rather than aborting or wrapping.
    auto const body = std::string { "5x1,0,0{2147483647x1,0,0,1,2147483647x1,2,0,2}" };
    auto const wire = std::format("{:04x},{}", layoutChecksum(body), body);
    auto const parsed = parseLayout(wire);
    REQUIRE_FALSE(parsed.has_value()); // rejected, and crucially without undefined behavior
    CHECK(parsed.error().contains("partition"));
}

TEST_CASE("pane-id backtracking distinguishes ids from sibling geometry", "[vthost][layout]")
{
    // No pane ids at all: after `79x50,0,0` the `,79x50,...` digits are the next
    // sibling's width, so the parser must rewind — precisely tmux's lookahead.
    auto const body = std::string { "159x50,0,0{79x50,0,0,79x50,80,0}" };
    auto const wire = std::format("{:04x},{}", layoutChecksum(body), body);
    auto const parsed = parseLayout(wire);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->children.size() == 2);
    CHECK_FALSE(parsed->children[0].paneId.has_value());
    CHECK_FALSE(parsed->children[1].paneId.has_value());
}

TEST_CASE("an n-ary container collapses into a right-leaning binary chain", "[vthost][layout]")
{
    // Even-horizontal over three panes: 53 + 1 + 52 + 1 + 53 == 160 columns.
    auto const body = std::string { "160x50,0,0{53x50,0,0,1,52x50,54,0,2,53x50,107,0,3}" };
    auto const wire = std::format("{:04x},{}", layoutChecksum(body), body);
    auto const parsed = parseLayout(wire);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->children.size() == 3);

    auto const binary = collapseToBinary(*parsed);
    CHECK(binary.leafCount() == 3);
    CHECK(binary.orientation == SplitState::Vertical);
    REQUIRE(binary.first != nullptr);
    CHECK(binary.first->paneId == 1);
    // ratio = 52 / 159 (the head's share of the whole container's divisible extent).
    CHECK(binary.ratio > 0.31);
    CHECK(binary.ratio < 0.34);

    auto const& tail = *binary.second;
    CHECK(tail.orientation == SplitState::Vertical);
    CHECK(tail.first->paneId == 2);
    CHECK(tail.second->paneId == 3);
    // tail ratio = 53 / 105 (its share of the REMAINING chain extent).
    CHECK(tail.ratio > 0.49);
    CHECK(tail.ratio < 0.52);
}

#ifndef _WIN32

    #include <unistd.h> // mkdtemp lives here on macOS (stdlib.h on glibc)

namespace
{

/// Runs a shell command, capturing stdout; returns nullopt if it failed.
[[nodiscard]] std::optional<std::string> capture(std::string const& command)
{
    auto* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr)
        return std::nullopt;
    auto output = std::string {};
    auto chunk = std::array<char, 256> {};
    while (::fgets(chunk.data(), chunk.size(), pipe) != nullptr)
        output += chunk.data();
    if (::pclose(pipe) != 0)
        return std::nullopt;
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return output;
}

[[nodiscard]] bool tmuxAvailable()
{
    return capture("command -v tmux 2>/dev/null").has_value();
}

/// A scoped private tmux server for oracle tests.
struct TmuxOracle
{
    std::string socket;

    TmuxOracle()
    {
        auto templ = (std::filesystem::temp_directory_path() / "cmux-oracle-XXXXXX").string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        socket = templ + "/tmux";
    }

    ~TmuxOracle()
    {
        std::ignore = capture(tmux("kill-server 2>/dev/null || true"));
        auto ec = std::error_code {};
        std::filesystem::remove_all(std::filesystem::path(socket).parent_path(), ec);
    }

    TmuxOracle(TmuxOracle const&) = delete;
    TmuxOracle& operator=(TmuxOracle const&) = delete;
    TmuxOracle(TmuxOracle&&) = delete;
    TmuxOracle& operator=(TmuxOracle&&) = delete;

    [[nodiscard]] std::string tmux(std::string const& arguments) const
    {
        return "tmux -S '" + socket + "' " + arguments;
    }
};

} // namespace

TEST_CASE("real tmux emits layout strings our parser accepts", "[vthost][layout][oracle]")
{
    if (!tmuxAvailable())
        SKIP("tmux not installed");

    auto const oracle = TmuxOracle {};
    REQUIRE(capture(oracle.tmux("new-session -d -x 160 -y 50")).has_value());
    REQUIRE(capture(oracle.tmux("split-window -h")).has_value());
    REQUIRE(capture(oracle.tmux("split-window -v")).has_value());

    auto const layout = capture(oracle.tmux("display-message -p '#{window_layout}'"));
    REQUIRE(layout.has_value());

    auto const parsed = parseLayout(*layout);
    REQUIRE(parsed.has_value());
    CHECK(collapseToBinary(*parsed).leafCount() == 3);
}

TEST_CASE("real tmux accepts every layout string we emit", "[vthost][layout][oracle]")
{
    if (!tmuxAvailable())
        SKIP("tmux not installed");

    // Three panes in tmux, three leaves in our tree: select-layout runs tmux's
    // own layout_check over our string — a genuine external oracle for the
    // cell arithmetic (sum + divider counts, cross-axis equality).
    auto const oracle = TmuxOracle {};
    REQUIRE(capture(oracle.tmux("new-session -d -x 160 -y 50")).has_value());
    REQUIRE(capture(oracle.tmux("split-window -h")).has_value());
    REQUIRE(capture(oracle.tmux("split-window -v")).has_value());

    auto tree = SampleTree {};
    auto const encoded = encodeLayout(tree.root, PageSize { LineCount(50), ColumnCount(160) });

    auto const applied = capture(oracle.tmux("select-layout '" + encoded + "' && echo applied"));
    REQUIRE(applied.has_value());
    CHECK(applied->contains("applied"));

    // And whatever tmux re-serialized after applying ours must parse again.
    auto const echoed = capture(oracle.tmux("display-message -p '#{window_layout}'"));
    REQUIRE(echoed.has_value());
    CHECK(parseLayout(*echoed).has_value());
}

#endif // !_WIN32

TEST_CASE("encodeLayout always emits a layout that passes layout_check", "[vthost][layout]")
{
    // splitCellExtents clamps both children to at least one cell, so an axis of 2 used to emit
    // children summing to 1 + 1 + 1 = 3 into a parent of 2. The result carries a VALID checksum but
    // an invalid geometry, and every control-mode consumer discards it — our own parseLayout says
    // "children do not partition the parent", and TmuxClientModel::ingestLayout then leaves the
    // previous state standing. The attached client's pane tree silently stops tracking the daemon:
    // newly split or closed panes never appear again.
    auto tree = SampleTree {};

    for (auto const lines: std::views::iota(1, 12))
    {
        for (auto const columns: std::views::iota(1, 12))
        {
            auto const encoded = encodeLayout(tree.root, PageSize { LineCount(lines), ColumnCount(columns) });
            auto const parsed = parseLayout(encoded);
            CAPTURE(columns, lines, encoded);
            REQUIRE(parsed.has_value());
        }
    }
}

TEST_CASE("encodeLayout widens an area too small for the tree", "[vthost][layout]")
{
    // The tree needs 2N-1 cells along an axis it stacks N leaves on; below that NO valid layout
    // string exists for it, so the encoder describes a slightly larger window instead — which is
    // what tmux itself does when the window exceeds the client, and what clients simply resize into.
    auto tree = SampleTree {}; // left | (top / bottom): 3 columns, 3 lines minimum

    auto const encoded = encodeLayout(tree.root, PageSize { LineCount(1), ColumnCount(1) });
    auto const parsed = parseLayout(encoded);
    REQUIRE(parsed.has_value());
    CHECK(parsed->width == 3);
    CHECK(parsed->height == 3);
    // Every leaf still owns at least one cell — a zero-extent pane is not a pane.
    REQUIRE(parsed->children.size() == 2);
    CHECK(parsed->children[0].width == 1);
    CHECK(parsed->children[1].children[0].height == 1);
    CHECK(parsed->children[1].children[1].height == 1);
}

TEST_CASE("encodeLayout leaves a sufficient area exactly as given", "[vthost][layout]")
{
    // The widening is a floor, never a rewrite: an area that can seat the tree is described as-is.
    auto tree = SampleTree {};
    auto const encoded = encodeLayout(tree.root, PageSize { LineCount(50), ColumnCount(160) });
    auto const parsed = parseLayout(encoded);
    REQUIRE(parsed.has_value());
    CHECK(parsed->width == 160);
    CHECK(parsed->height == 50);
}

TEST_CASE("encodeLayout keeps a deep nested tree partitioned", "[vthost][layout]")
{
    // A chain of splits along ONE axis is where a per-child minimum of one cell is not enough: an
    // outer split can hand its second child three cells while that child's own subtree needs seven.
    auto root = Pane { PaneId { 1 }, SessionId { 100 } };
    auto* node = &root;
    for (auto const level: std::views::iota(std::uint64_t { 0 }, std::uint64_t { 5 }))
        node = node->split(SplitState::Horizontal,
                           PaneId { 10 + level },
                           PaneId { 20 + level },
                           SessionId { 200 + level },
                           0.5)
                   .second;

    for (auto const lines: std::views::iota(1, 20))
    {
        auto const encoded = encodeLayout(root, PageSize { LineCount(lines), ColumnCount(80) });
        CAPTURE(lines, encoded);
        REQUIRE(parseLayout(encoded).has_value());
    }
}

namespace
{

/// Builds a checksummed layout string for a flat side-by-side container of @p leaves one-cell panes.
///
/// The shape a hostile peer reaches for: the container's NESTING is one level (so MaxNestingDepth
/// never fires) while its BREADTH is unbounded, and every leaf satisfies layout_check because
/// `sum(width + 1) - 1` equals the parent width by construction.
std::string wideContainerLayout(int leaves)
{
    auto body = std::format("{}x1,0,0{{", (2 * leaves) - 1);
    for (auto const i: std::views::iota(0, leaves))
    {
        if (i != 0)
            body += ',';
        body += std::format("1x1,{},0,{}", 2 * i, i + 1);
    }
    body += '}';
    return std::format("{:04x},{}", layoutChecksum(body), body);
}

} // namespace

// A flat container with very many children is accepted by the grammar and by layout_check, and the
// collapse pass turns a container's tail into a right-leaning CHAIN -- so building it, walking it and
// destroying it were all one frame per sibling. MaxNestingDepth bounds only the depth axis, so a
// ~1 MiB %layout-change naming ~131,000 one-cell leaves (well inside the reader's line limit)
// overflowed the stack. The breadth axis is now bounded too.
TEST_CASE("parseLayout rejects a layout with too many nodes", "[vthost][layout]")
{
    // Comfortably inside the cap: accepted, and the collapse must survive it.
    auto const modest = wideContainerLayout(64);
    auto const parsedModest = parseLayout(modest);
    REQUIRE(parsedModest.has_value());
    CHECK(parsedModest->children.size() == 64);
    CHECK(collapseToBinary(*parsedModest).leafCount() == 64);

    // Past it: rejected like any other malformed layout, rather than accepted and then collapsed
    // into a 5000-deep unique_ptr chain.
    auto const hostile = parseLayout(wideContainerLayout(5000));
    REQUIRE_FALSE(hostile.has_value());
    CHECK(hostile.error() == "layout has too many nodes");
}

// The collapse itself must not recurse per sibling either -- the cap above is a policy, not the
// thing that keeps the stack safe. A container at the very edge of what is accepted collapses fine.
TEST_CASE("collapseToBinary handles a wide container without recursing per sibling", "[vthost][layout]")
{
    auto const parsed = parseLayout(wideContainerLayout(1000));
    REQUIRE(parsed.has_value());

    auto const tree = collapseToBinary(*parsed);
    CHECK(tree.leafCount() == 1000);
    // Right-leaning: the head is the first leaf, the tail is everything after it.
    REQUIRE(tree.first != nullptr);
    REQUIRE(tree.second != nullptr);
    CHECK(tree.first->paneId == std::uint64_t { 1 });
    CHECK(tree.second->leafCount() == 999);
    CHECK(tree.orientation == SplitState::Vertical);
}

// The ratios a chain derives must not change with the rewrite from recursion to iteration: each
// chain node's first child takes its own extent's share of what the node still spans.
TEST_CASE("collapseToBinary derives the same chain ratios as the layout states", "[vthost][layout]")
{
    // `{a,b,c}` with widths 9, 9, 9 in a 29-wide parent: a takes 9 of 29-1, then b takes 9 of 19-1.
    auto const body = std::string { "29x10,0,0{9x10,0,0,1,9x10,10,0,2,9x10,20,0,3}" };
    auto const text = std::format("{:04x},{}", layoutChecksum(body), body);
    auto const parsed = parseLayout(text);
    REQUIRE(parsed.has_value());

    auto const tree = collapseToBinary(*parsed);
    REQUIRE(tree.first != nullptr);
    REQUIRE(tree.second != nullptr);
    CHECK(tree.first->paneId == std::uint64_t { 1 });
    CHECK(tree.ratio == 9.0 / 28.0);
    CHECK(tree.second->ratio == 9.0 / 18.0);
    CHECK(tree.second->first->paneId == std::uint64_t { 2 });
    CHECK(tree.second->second->paneId == std::uint64_t { 3 });
}

// collapseToBinary is a public entry point, so it must not index an empty child vector for a
// container parseLayout could never have produced.
TEST_CASE("collapseToBinary treats a childless container as a leaf", "[vthost][layout]")
{
    auto node = ParsedLayout {};
    node.kind = ParsedLayout::Kind::SideBySide;
    auto const tree = collapseToBinary(node);
    CHECK(tree.leafCount() == 1);
    CHECK(tree.first == nullptr);
}
