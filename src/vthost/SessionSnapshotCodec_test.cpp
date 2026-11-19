// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <vthost/SessionSnapshotCodec.hpp>
#include <vthost/proto/Wire.hpp>
#include <vthost/testing/RecordedNameEncodings.hpp>
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

[[nodiscard]] ObjectIdentityRecord identityOf(std::string name)
{
    auto record = ObjectIdentityRecord { .name = std::move(name) };
    std::ranges::generate(record.uuid, [next = std::uint8_t { 1 }]() mutable { return next++; });
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
    auto const plain = WorkspaceLayoutSnapshot { .tabs = { TabSnapshot { .root = leaf() } } };

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

    auto second = WorkspaceLayoutSnapshot {};
    second.identity = identityOf("api");
    second.tabs.push_back(std::move(tab));
    second.activeTab = 0;

    auto workspace = WorkspaceSnapshot {};
    workspace.takenAt = std::chrono::sys_seconds { std::chrono::seconds { 1'761'000'000 } };
    workspace.workspaces.push_back(plain);
    workspace.workspaces.push_back(std::move(second));
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
    walk(workspace.workspaces.at(1).tabs.at(0).root);
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

/// The body of a one-workspace, one-tab file whose tab blob is @p tab.
[[nodiscard]] std::vector<std::byte> oneTabFile(std::span<std::byte const> tab)
{
    auto workspace = proto::Writer {};
    workspace.u8(0); // no identity
    workspace.varint(0);
    workspace.varint(1);
    workspace.blob(tab);
    auto body = proto::Writer {};
    body.svarint(1000);
    body.varint(1);
    body.blob(workspace.view());
    return fileAround(body.view());
}

/// A tab blob around a single-leaf node whose leaf is written by @p leaf.
template <typename WriteLeaf>
[[nodiscard]] std::vector<std::byte> tabBlob(WriteLeaf&& writeLeaf, std::size_t trailing = 0)
{
    auto node = proto::Writer {};
    node.u8(0); // leaf
    std::forward<WriteLeaf>(writeLeaf)(node);
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

/// @return A split tree as balanced as it gets with @p leaves leaves, so it stays shallow however many it
/// has.
[[nodiscard]] PaneNode splitsWithLeaves(std::size_t leaves)
{
    if (leaves <= 1)
        return leaf();
    return split(vtworkspace::SplitState::Horizontal,
                 0.5,
                 splitsWithLeaves(leaves / 2),
                 splitsWithLeaves(leaves - (leaves / 2)));
}

[[nodiscard]] WorkspaceSnapshot workspaceOf(PaneNode root)
{
    auto workspace = WorkspaceSnapshot {};
    workspace.workspaces.push_back(
        WorkspaceLayoutSnapshot { .tabs = { TabSnapshot { .root = std::move(root) } } });
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

    auto const& tab = decoded->snapshot.workspaces.at(1).tabs.at(0);
    CHECK(tab.title == "build");
    CHECK(tab.color == vtbackend::RGBColor { 0x336699U });
    CHECK(tab.activeLeaf == 2);
    CHECK(tab.zoom == TabZoom::Zoomed);
    CHECK(tab.root.split == vtworkspace::SplitState::Vertical);
    CHECK(tab.root.ratio == 0.3);
    CHECK(tab.identity.value().name == "build-tab");
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

TEST_CASE("a workspace's trailing visibility byte, written by an older daemon, is skipped",
          "[vthost][snapshot][visibility]")
{
    // The daemon no longer records what its clients showed: a snapshot written before carries one byte
    // after the tabs, and what follows the tabs of a workspace's record is never read.
    auto workspace = proto::Writer {};
    workspace.u8(0); // identity
    workspace.varint(2);
    workspace.varint(0);
    workspace.u8(1); // the retired visibility byte (Hidden)
    auto body = proto::Writer {};
    body.svarint(5);
    body.varint(1);
    body.blob(workspace.view());
    auto const decoded = decodeSnapshot(fileAround(body.view()));
    REQUIRE(decoded);
    CHECK(decoded->snapshot.workspaces.at(0).activeTab == 2);
    CHECK(decoded->snapshot.workspaces.at(0).tabs.empty());
    // The byte sits where the colour's filler goes: it is never read as a colour.
    CHECK_FALSE(decoded->snapshot.workspaces.at(0).color.has_value());
}

TEST_CASE("a workspace's colour round-trips, and a workspace without one encodes as before",
          "[vthost][snapshot][workspace-color]")
{
    auto original = sampleWorkspace();
    original.workspaces.at(1).color = vtbackend::RGBColor { 0x7A3E9DU };
    auto const decoded = decodeSnapshot(encodeSnapshot(original));
    REQUIRE(decoded);
    CHECK(decoded->snapshot == original);
    CHECK(decoded->snapshot.workspaces.at(0).color == std::nullopt);
    CHECK(decoded->snapshot.workspaces.at(1).color == vtbackend::RGBColor { 0x7A3E9DU });

    // Without a colour nothing follows the tabs, so the file is byte for byte what a daemon before the colour
    // wrote, and that daemon reads a coloured one with the colour skipped.
    original.workspaces.at(1).color.reset();
    CHECK(encodeSnapshot(original) == encodeSnapshot(sampleWorkspace()));
}

TEST_CASE("a workspace's colour follows a filler byte in the retired visibility byte's slot",
          "[vthost][snapshot][workspace-color]")
{
    auto workspace = proto::Writer {};
    workspace.u8(0); // identity
    workspace.varint(0);
    workspace.varint(0);
    workspace.u8(0); // filler: what an older daemon read as its visibility byte (Left)
    workspace.u8(1); // has a colour
    workspace.u32(0x336699U);
    auto body = proto::Writer {};
    body.svarint(5);
    body.varint(1);
    body.blob(workspace.view());
    auto const file = fileAround(body.view());

    auto const decoded = decodeSnapshot(file);
    REQUIRE(decoded);
    CHECK(decoded->snapshot.workspaces.at(0).color == vtbackend::RGBColor { 0x336699U });

    auto coloured = WorkspaceSnapshot { .takenAt = std::chrono::sys_seconds { std::chrono::seconds { 5 } } };
    coloured.workspaces.push_back(WorkspaceLayoutSnapshot { .color = vtbackend::RGBColor { 0x336699U } });
    CHECK(encodeSnapshot(coloured) == file);
}

TEST_CASE("a workspace's colour cut short is Malformed", "[vthost][snapshot][workspace-color]")
{
    auto const fileWithColorBytes = [](std::size_t colorBytes) {
        auto workspace = proto::Writer {};
        workspace.u8(0); // identity
        workspace.varint(0);
        workspace.varint(0);
        workspace.u8(0); // filler
        workspace.u8(1); // has a colour
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, colorBytes))
            workspace.u8(0x99);
        auto body = proto::Writer {};
        body.svarint(5);
        body.varint(1);
        body.blob(workspace.view());
        return fileAround(body.view());
    };

    SECTION("no colour bytes")
    {
        CHECK(decodeSnapshot(fileWithColorBytes(0)).error() == SnapshotError::Malformed);
    }
    SECTION("two of the four colour bytes")
    {
        CHECK(decodeSnapshot(fileWithColorBytes(2)).error() == SnapshotError::Malformed);
    }
    SECTION("all four, for contrast")
    {
        auto const decoded = decodeSnapshot(fileWithColorBytes(4));
        REQUIRE(decoded);
        CHECK(decoded->snapshot.workspaces.at(0).color == vtbackend::RGBColor { 0x999999U });
    }
}

TEST_CASE("a snapshot written before the workspace colour decodes without one",
          "[vthost][snapshot][workspace-color]")
{
    for (auto const recorded:
         { std::span<std::byte const> { testing::RecordedSnapshotV1 },
           std::span<std::byte const> { std::as_bytes(std::span { testing::RecordedSnapshotNames }) } })
    {
        auto const decoded = decodeSnapshot(recorded);
        REQUIRE(decoded);
        REQUIRE_FALSE(decoded->snapshot.workspaces.empty());
        for (auto const& workspace: decoded->snapshot.workspaces)
            CHECK_FALSE(workspace.color.has_value());
    }
}

TEST_CASE("a learned cell size round-trips, and a file without one decodes to none", "[vthost][snapshot]")
{
    auto original = sampleWorkspace();
    REQUIRE_FALSE(original.cellPixelSize);

    SECTION("engaged")
    {
        original.cellPixelSize = vtpty::ImageSize { vtpty::Width(9), vtpty::Height(18) };
        auto const decoded = decodeSnapshot(encodeSnapshot(original));
        REQUIRE(decoded);
        CHECK(decoded->snapshot == original);
        CHECK(decoded->snapshot.cellPixelSize == original.cellPixelSize);
    }
    SECTION("absent")
    {
        auto const decoded = decodeSnapshot(encodeSnapshot(original));
        REQUIRE(decoded);
        CHECK_FALSE(decoded->snapshot.cellPixelSize);
    }
    SECTION("a file written before the field")
    {
        auto const decoded = decodeSnapshot(testing::RecordedSnapshotV1);
        REQUIRE(decoded);
        CHECK_FALSE(decoded->snapshot.cellPixelSize);
    }
}

TEST_CASE("a damaged trailing cell size drops only the cell", "[vthost][snapshot]")
{
    auto body = proto::Writer {};
    body.svarint(0);
    body.varint(0); // no workspaces

    SECTION("a width and no height")
    {
        body.varint(9);
        auto const decoded = decodeSnapshot(fileAround(body.view()));
        REQUIRE(decoded);
        CHECK(decoded->snapshot.workspaces.empty());
        CHECK_FALSE(decoded->snapshot.cellPixelSize);
    }
    SECTION("an oversized extent decodes out of bounds, never negative")
    {
        body.varint(uint64_t { UINT32_MAX } + 1);
        body.varint(18);
        auto const decoded = decodeSnapshot(fileAround(body.view()));
        REQUIRE(decoded);
        REQUIRE(decoded->snapshot.cellPixelSize);
        CHECK(unbox<int64_t>(decoded->snapshot.cellPixelSize->width) > 1024);
        CHECK(unbox<int64_t>(decoded->snapshot.cellPixelSize->height) == 18);
    }
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
        lopsided.workspaces[0].tabs[0].root.children.pop_back();
        CHECK(decodeSnapshot(encodeSnapshot(lopsided)).error() == SnapshotError::Malformed);
    }
    SECTION("65 workspaces")
    {
        auto many = WorkspaceSnapshot {};
        many.workspaces.resize(MaxSnapshotWorkspaces + 1);
        CHECK(decodeSnapshot(encodeSnapshot(many)).error() == SnapshotError::Malformed);
        many.workspaces.pop_back();
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
        // Balanced, so that the leaf count is what is refused, not the depth: a chain of that many splits
        // is far deeper than MaxPaneDepth, and deep enough to overflow a Windows thread's stack.
        CHECK(decodeSnapshot(encodeSnapshot(workspaceOf(splitsWithLeaves(MaxLeavesPerTab)))));
        CHECK(decodeSnapshot(encodeSnapshot(workspaceOf(splitsWithLeaves(MaxLeavesPerTab + 1)))).error()
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
        CHECK(decoded->snapshot.workspaces.at(0).tabs.size() == 1);
        CHECK(decoded->snapshot.workspaces[0].tabs[0].root.split == vtworkspace::SplitState::None);
    }
    SECTION("a workspace that ends after its identity")
    {
        auto workspace = proto::Writer {};
        workspace.u8(0);
        auto body = proto::Writer {};
        body.svarint(5);
        body.varint(1);
        body.blob(workspace.view());
        auto const decoded = decodeSnapshot(fileAround(body.view()));
        REQUIRE(decoded);
        CHECK(decoded->snapshot.workspaces.at(0).activeTab == 0);
        CHECK(decoded->snapshot.workspaces[0].tabs.empty());
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
        auto const& pane = decoded->snapshot.workspaces.at(0).tabs.at(0).root.pane;
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
    auto const& pane = decoded->snapshot.workspaces.at(0).tabs.at(0).root.pane;
    CHECK_FALSE(pane.history.has_value());
    CHECK(pane.workingDirectory == fs::path { "/srv" });
}

namespace
{

/// A snapshot whose every name-bearing field is set: the workspace's, the tab's and the pane's name, and the
/// tab's runtime title. Recorded as testing::RecordedSnapshotNames.
[[nodiscard]] WorkspaceSnapshot namedSnapshot()
{
    auto tab = TabSnapshot {};
    tab.identity = identityOf("build");
    tab.title = "edited {Cwd}";
    tab.root = leaf(PaneSnapshot { .identity = identityOf("logs") });

    auto workspace = WorkspaceLayoutSnapshot {};
    workspace.identity = identityOf("ops");
    workspace.tabs.push_back(std::move(tab));

    auto snapshot = WorkspaceSnapshot {};
    snapshot.takenAt = std::chrono::sys_seconds { std::chrono::seconds { 1'763'000'000 } };
    snapshot.workspaces.push_back(std::move(workspace));
    return snapshot;
}

} // namespace

TEST_CASE("the snapshot file of a named workspace, tab and pane is pinned",
          "[vthost][snapshot][encoding-pin]")
{
    auto const snapshot = namedSnapshot();
    auto const encoded = encodeSnapshot(snapshot);
    if (testing::isFixtureUpdateRequested(core::LiveEnvironment {}))
    {
        std::cout << testing::spelledFixture("RecordedSnapshotNames", encoded);
        FAIL("printed RecordedSnapshotNames instead of comparing; paste it into RecordedNameEncodings.hpp");
    }
    auto const recorded = std::as_bytes(std::span { testing::RecordedSnapshotNames });
    CHECK(std::ranges::equal(encoded, recorded));

    auto const decoded = decodeSnapshot(recorded);
    REQUIRE(decoded);
    CHECK(decoded->droppedHistories == 0);
    CHECK(decoded->snapshot == snapshot);
    REQUIRE(decoded->snapshot.workspaces.size() == 1);
    auto const& workspace = decoded->snapshot.workspaces.at(0);
    CHECK(workspace.identity.value().name == "ops");
    REQUIRE(workspace.tabs.size() == 1);
    CHECK(workspace.tabs.at(0).identity.value().name == "build");
    CHECK(workspace.tabs.at(0).title == "edited {Cwd}");
    CHECK(workspace.tabs.at(0).root.pane.identity.value().name == "logs");
}
