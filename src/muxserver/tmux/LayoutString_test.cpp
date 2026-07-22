// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PageSize.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>

#include <muxserver/tmux/LayoutString.h>
#include <vtmux/Pane.h>

using muxserver::tmux::BinaryLayout;
using muxserver::tmux::collapseToBinary;
using muxserver::tmux::encodeLayout;
using muxserver::tmux::layoutChecksum;
using muxserver::tmux::ParsedLayout;
using muxserver::tmux::parseLayout;
using vtmux::Pane;
using vtmux::PaneId;
using vtmux::SessionId;
using vtmux::SplitState;
using vtpty::ColumnCount;
using vtpty::LineCount;
using vtpty::PageSize;

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

TEST_CASE("layout checksum matches tmux's rotate-add algorithm", "[muxserver][layout]")
{
    // Manually computed over "ab": 'a' (97), then rotate(97)=48+... — assert the
    // algebra rather than a magic value: rotate-right of odd sets the top bit.
    CHECK(layoutChecksum("") == 0);
    CHECK(layoutChecksum("a") == 97);
    CHECK(layoutChecksum("b") == 98);
    // rotate(97) = (97>>1) | (1<<15) = 48 + 32768; + 'b'(98) = 32914
    CHECK(layoutChecksum("ab") == 32914);
}

TEST_CASE("encodeLayout emits geometry the parser round-trips as a tree", "[muxserver][layout]")
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

TEST_CASE("a corrupted checksum is rejected", "[muxserver][layout]")
{
    auto tree = SampleTree {};
    auto encoded = encodeLayout(tree.root, PageSize { LineCount(50), ColumnCount(160) });
    encoded[0] = encoded[0] == '0' ? '1' : '0';
    CHECK_FALSE(parseLayout(encoded).has_value());
}

TEST_CASE("a layout violating the partition arithmetic is rejected", "[muxserver][layout]")
{
    // 80 + 1 + 80 != 160: layout_check must refuse, exactly like tmux.
    auto const body = std::string { "160x50,0,0{80x50,0,0,1,80x50,81,0,2}" };
    auto const wire = std::format("{:04x},{}", layoutChecksum(body), body);
    auto const parsed = parseLayout(wire);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().contains("partition"));
}

TEST_CASE("a pathologically nested layout is refused, not crashed", "[muxserver][layout]")
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

TEST_CASE("layout dimensions near INT_MAX do not overflow the partition check", "[muxserver][layout]")
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

TEST_CASE("pane-id backtracking distinguishes ids from sibling geometry", "[muxserver][layout]")
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

TEST_CASE("an n-ary container collapses into a right-leaning binary chain", "[muxserver][layout]")
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

TEST_CASE("real tmux emits layout strings our parser accepts", "[muxserver][layout][oracle]")
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

TEST_CASE("real tmux accepts every layout string we emit", "[muxserver][layout][oracle]")
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
