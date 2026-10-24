// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <vthost/SessionSnapshotCodec.hpp>
#include <vthost/proto/Wire.hpp>
#include <vthost/testing/RecordedSnapshots.hpp>

using namespace vthost;
namespace fs = std::filesystem;

namespace
{

[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    auto const view = std::as_bytes(std::span { text.data(), text.size() });
    return { view.begin(), view.end() };
}

[[nodiscard]] ObjectIdentityRecord identityOf(std::string label)
{
    auto record = ObjectIdentityRecord { .label = std::move(label) };
    std::iota(record.uuid.begin(), record.uuid.end(), std::uint8_t { 1 });
    return record;
}

[[nodiscard]] PaneNode leaf(PaneSnapshot pane = {})
{
    return PaneNode { .pane = std::move(pane) };
}

[[nodiscard]] PaneNode split(vtworkspace::SplitState state, double ratio, PaneNode first, PaneNode second)
{
    auto node = PaneNode { .split = state, .ratio = ratio };
    node.children.push_back(std::move(first));
    node.children.push_back(std::move(second));
    return node;
}

[[nodiscard]] proto::Delta historyDelta()
{
    auto delta = proto::Delta {};
    delta.session = 7;
    delta.snapshot = 1;
    delta.stableFloor = -3;
    for (auto const index: std::views::iota(0, 3))
    {
        auto line = proto::WireLine {};
        line.stableId = index - 3;
        line.blockId = index == 1 ? 1U : 0U;
        line.columns = 80;
        delta.lines.push_back(std::move(line));
    }
    delta.hyperlinks.push_back(proto::HyperlinkEntry { .id = 1, .uri = "https://contour-terminal.org" });
    auto block = proto::WireCommandBlock {};
    block.id = 1;
    block.present = proto::CommandBlockHasExitCode;
    block.commandLine = "make test";
    block.outcomeStatus = 2;
    block.exitCode = 2;
    delta.commandBlocks.push_back(std::move(block));
    return delta;
}

/// The workspace the round trip and the recorded format-1 file both stand for.
[[nodiscard]] WorkspaceSnapshot sampleWorkspace()
{
    auto const plain = WindowSnapshot { .tabs = { TabSnapshot { .root = leaf() } } };

    auto editor = PaneSnapshot {};
    editor.workingDirectory = fs::path { u8"/home/me/Größe" };
    editor.profile = "work";
    editor.foregroundProcess = "vim";
    editor.runningCommandLine = "vim notes.md";
    editor.history = historyDelta();

    auto tab = TabSnapshot {};
    tab.identity = identityOf("build-tab");
    tab.title = "build";
    tab.color = vtbackend::RGBColor { 0x336699U };
    tab.root = split(vtworkspace::SplitState::Vertical,
                     0.3,
                     leaf(),
                     split(vtworkspace::SplitState::Horizontal,
                           0.5,
                           leaf(PaneSnapshot { .identity = identityOf("api") }),
                           leaf(std::move(editor))));
    tab.activeLeaf = 2;
    tab.zoom = TabZoom::Zoomed;

    auto second = WindowSnapshot {};
    second.identity = identityOf("api");
    second.tabs.push_back(std::move(tab));
    second.activeTab = 0;

    auto workspace = WorkspaceSnapshot {};
    workspace.takenAt = std::chrono::sys_seconds { std::chrono::seconds { 1'761'000'000 } };
    workspace.windows.push_back(plain);
    workspace.windows.push_back(std::move(second));
    return workspace;
}

[[nodiscard]] PaneSnapshot const& leafOf(WorkspaceSnapshot const& workspace, std::size_t index)
{
    auto leaves = std::vector<PaneNode const*> {};
    auto const walk = [&](this auto const& self, PaneNode const& node) -> void {
        if (node.split == vtworkspace::SplitState::None)
            leaves.push_back(&node);
        for (auto const& child: node.children)
            self(child);
    };
    walk(workspace.windows.at(1).tabs.at(0).root);
    return leaves.at(index)->pane;
}

/// Wraps @p body in a well-formed file header.
[[nodiscard]] std::vector<std::byte> fileAround(std::span<std::byte const> body,
                                                std::uint64_t version = SnapshotFormatVersion)
{
    auto file = proto::Writer {};
    file.bytes(bytesOf("CTRSNAP\n"));
    file.varint(version);
    file.varint(body.size());
    auto const checksum = fnv1a64(body);
    file.u32(static_cast<std::uint32_t>(checksum));
    file.u32(static_cast<std::uint32_t>(checksum >> 32));
    file.bytes(body);
    return file.take();
}

/// The body of a one-window, one-tab file whose tab blob is @p tab.
[[nodiscard]] std::vector<std::byte> oneTabFile(std::span<std::byte const> tab)
{
    auto window = proto::Writer {};
    window.u8(0); // no identity
    window.varint(0);
    window.varint(1);
    window.blob(tab);
    auto body = proto::Writer {};
    body.svarint(1000);
    body.varint(1);
    body.blob(window.view());
    return fileAround(body.view());
}

/// A tab blob around a single-leaf node whose leaf is written by @p leaf.
template <typename WriteLeaf>
[[nodiscard]] std::vector<std::byte> tabBlob(WriteLeaf&& writeLeaf, std::size_t trailing = 0)
{
    auto node = proto::Writer {};
    node.u8(0); // leaf
    writeLeaf(node);
    auto tab = proto::Writer {};
    tab.u8(0); // identity
    tab.u8(0); // title
    tab.u8(0); // colour
    tab.varint(0);
    tab.u8(0);
    tab.blob(node.view());
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, trailing))
        tab.u8(0xAB);
    return tab.take();
}

[[nodiscard]] PaneNode nestedSplits(std::size_t depth)
{
    return depth == 0 ? leaf()
                      : split(vtworkspace::SplitState::Horizontal, 0.5, leaf(), nestedSplits(depth - 1));
}

[[nodiscard]] WorkspaceSnapshot workspaceOf(PaneNode root)
{
    auto workspace = WorkspaceSnapshot {};
    workspace.windows.push_back(WindowSnapshot { .tabs = { TabSnapshot { .root = std::move(root) } } });
    return workspace;
}

} // namespace

TEST_CASE("fnv1a64 matches the reference vectors", "[vthost][snapshot]")
{
    CHECK(fnv1a64({}) == 0xcbf29ce484222325ULL);
    CHECK(fnv1a64(bytesOf("a")) == 0xaf63dc4c8601ec8cULL);
    CHECK(fnv1a64(bytesOf("foobar")) == 0x85944171f73967e8ULL);
}

TEST_CASE("a workspace round-trips through the codec", "[vthost][snapshot]")
{
    auto const original = sampleWorkspace();
    auto const decoded = decodeSnapshot(encodeSnapshot(original));
    REQUIRE(decoded);
    CHECK(decoded->droppedHistories == 0);
    CHECK(decoded->snapshot == original);

    auto const& tab = decoded->snapshot.windows.at(1).tabs.at(0);
    CHECK(tab.title == "build");
    CHECK(tab.color == vtbackend::RGBColor { 0x336699U });
    CHECK(tab.activeLeaf == 2);
    CHECK(tab.zoom == TabZoom::Zoomed);
    CHECK(tab.root.split == vtworkspace::SplitState::Vertical);
    CHECK(tab.root.ratio == 0.3);
    CHECK(tab.identity->label == "build-tab");
    CHECK(tab.identity->uuid.front() == 1);
    CHECK(tab.identity->uuid.back() == 16);

    auto const& pane = leafOf(decoded->snapshot, 2);
    CHECK(pane.workingDirectory == fs::path { u8"/home/me/Größe" });
    CHECK(pane.profile == "work");
    CHECK(pane.foregroundProcess == "vim");
    CHECK(pane.runningCommandLine == "vim notes.md");
    REQUIRE(pane.history);
    CHECK(pane.history->lines.size() == 3);
    CHECK(pane.history->hyperlinks.size() == 1);
    REQUIRE(pane.history->commandBlocks.size() == 1);
    CHECK(pane.history->commandBlocks[0].exitCode == 2);
}

TEST_CASE("an empty workspace encodes and decodes as empty", "[vthost][snapshot]")
{
    auto const decoded = decodeSnapshot(encodeSnapshot({}));
    REQUIRE(decoded);
    CHECK(decoded->snapshot.empty());
}

TEST_CASE("decodeSnapshot names what is wrong with a damaged file", "[vthost][snapshot]")
{
    auto const good = encodeSnapshot(sampleWorkspace());

    SECTION("first byte changed")
    {
        auto file = good;
        file[0] = std::byte { 'X' };
        CHECK(decodeSnapshot(file).error() == SnapshotError::BadMagic);
    }
    SECTION("version varint 2")
    {
        auto file = good;
        file[8] = std::byte { 2 };
        CHECK(decodeSnapshot(file).error() == SnapshotError::UnsupportedFormat);
    }
    SECTION("file cut one byte before its end")
    {
        CHECK(decodeSnapshot(std::span { good }.first(good.size() - 1)).error() == SnapshotError::Truncated);
    }
    SECTION("file cut inside the header")
    {
        CHECK(decodeSnapshot(std::span { good }.first(5)).error() == SnapshotError::Truncated);
        CHECK(decodeSnapshot(std::span { good }.first(10)).error() == SnapshotError::Truncated);
    }
    SECTION("one body byte flipped")
    {
        auto file = good;
        file.back() ^= std::byte { 0x01 };
        CHECK(decodeSnapshot(file).error() == SnapshotError::ChecksumMismatch);
    }
    SECTION("a split node with one child")
    {
        auto lopsided = workspaceOf(split(vtworkspace::SplitState::Vertical, 0.5, leaf(), leaf()));
        lopsided.windows[0].tabs[0].root.children.pop_back();
        CHECK(decodeSnapshot(encodeSnapshot(lopsided)).error() == SnapshotError::Malformed);
    }
    SECTION("65 windows")
    {
        auto many = WorkspaceSnapshot {};
        many.windows.resize(MaxSnapshotWindows + 1);
        CHECK(decodeSnapshot(encodeSnapshot(many)).error() == SnapshotError::Malformed);
        many.windows.pop_back();
        CHECK(decodeSnapshot(encodeSnapshot(many)));
    }
    SECTION("pane nesting deeper than 32")
    {
        CHECK(decodeSnapshot(encodeSnapshot(workspaceOf(nestedSplits(MaxPaneDepth)))));
        CHECK(decodeSnapshot(encodeSnapshot(workspaceOf(nestedSplits(MaxPaneDepth + 1)))).error()
              == SnapshotError::Malformed);
    }
    SECTION("more leaves than a tab may hold")
    {
        CHECK(decodeSnapshot(encodeSnapshot(workspaceOf(nestedSplits(MaxLeavesPerTab - 1)))).error()
              == SnapshotError::Malformed);
    }
    SECTION("a ratio of 0 or 1")
    {
        for (auto const ratio: { 0.0, 1.0 })
            CHECK(decodeSnapshot(encodeSnapshot(workspaceOf(
                                     split(vtworkspace::SplitState::Vertical, ratio, leaf(), leaf()))))
                      .error()
                  == SnapshotError::Malformed);
    }
    SECTION("a file past the size limit")
    {
        auto const huge = std::vector<std::byte>(MaxSnapshotFileBytes + 1);
        CHECK(decodeSnapshot(huge).error() == SnapshotError::TooLarge);
    }
}

TEST_CASE("a record's unknown trailing fields are skipped and absent ones defaulted", "[vthost][snapshot]")
{
    SECTION("three extra bytes after a tab's node")
    {
        auto const file = oneTabFile(tabBlob([](proto::Writer& node) { node.u8(0); }, 3));
        auto const decoded = decodeSnapshot(file);
        REQUIRE(decoded);
        CHECK(decoded->snapshot.windows.at(0).tabs.size() == 1);
        CHECK(decoded->snapshot.windows[0].tabs[0].root.split == vtworkspace::SplitState::None);
    }
    SECTION("a window that ends after its identity")
    {
        auto window = proto::Writer {};
        window.u8(0);
        auto body = proto::Writer {};
        body.svarint(5);
        body.varint(1);
        body.blob(window.view());
        auto const decoded = decodeSnapshot(fileAround(body.view()));
        REQUIRE(decoded);
        CHECK(decoded->snapshot.windows.at(0).activeTab == 0);
        CHECK(decoded->snapshot.windows[0].tabs.empty());
    }
    SECTION("a leaf that ends after its working directory")
    {
        auto const file = oneTabFile(tabBlob([](proto::Writer& node) {
            node.u8(0); // identity
            node.u8(1);
            node.string("/tmp");
        }));
        auto const decoded = decodeSnapshot(file);
        REQUIRE(decoded);
        auto const& pane = decoded->snapshot.windows.at(0).tabs.at(0).root.pane;
        CHECK(pane.workingDirectory == fs::path { "/tmp" });
        CHECK_FALSE(pane.profile);
        CHECK_FALSE(pane.history);
    }
}

TEST_CASE("a recorded format-1 snapshot still decodes", "[vthost][snapshot]")
{
    // The bytes were written once by this task's encoder and are never regenerated: a change that breaks
    // this test broke the drift restart, where the old daemon writes and the new one reads.
    auto const decoded = decodeSnapshot(testing::RecordedSnapshotV1);
    REQUIRE(decoded);
    CHECK(decoded->droppedHistories == 0);
    CHECK(decoded->snapshot == sampleWorkspace());
}

TEST_CASE("a pane whose history does not decode loses only its history", "[vthost][snapshot]")
{
    auto const file = oneTabFile(tabBlob([](proto::Writer& node) {
        node.u8(0); // identity
        node.u8(1);
        node.string("/srv");
        node.u8(0); // profile
        node.u8(0); // foreground
        node.u8(0); // command line
        node.u8(1);
        node.blob(bytesOf("junk!"));
    }));
    auto const decoded = decodeSnapshot(file);
    REQUIRE(decoded);
    CHECK(decoded->droppedHistories == 1);
    auto const& pane = decoded->snapshot.windows.at(0).tabs.at(0).root.pane;
    CHECK_FALSE(pane.history.has_value());
    CHECK(pane.workingDirectory == fs::path { "/srv" });
}
