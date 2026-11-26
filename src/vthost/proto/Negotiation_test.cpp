// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

#include <vthost/proto/ControlVersions.hpp>
#include <vthost/proto/Negotiation.hpp>

using namespace vthost::proto;

TEST_CASE("negotiate picks the highest common version", "[vthost][negotiation]")
{
    CHECK(negotiate({ 4, 6 }, { 5, 7 }) == 6u);
    CHECK(negotiate({ 4, 4 }, { 4, 9 }) == 4u);
}

TEST_CASE("negotiate refuses disjoint and malformed ranges, naming both", "[vthost][negotiation]")
{
    auto const disjoint = negotiate({ 4, 5 }, { 6, 8 });
    REQUIRE_FALSE(disjoint);
    CHECK(disjoint.error() == HandshakeRefusal { .ours = { 4, 5 }, .theirs = { 6, 8 } });
    CHECK_FALSE(negotiate({ 4, 5 }, { 5, 4 })); // min > max
    CHECK_FALSE(negotiate({ 4, 5 }, { 3, 3 })); // pre-negotiation peer
}

TEST_CASE("isPreNegotiation compares against the hello marker", "[vthost][negotiation]")
{
    CHECK(isPreNegotiation({ 3, 3 }));
    CHECK_FALSE(isPreNegotiation({ 3, 4 }));
    CHECK_FALSE(isPreNegotiation(SupportedVersions));
}

TEST_CASE("describe names both ranges and says when a peer predates negotiation", "[vthost][negotiation]")
{
    CHECK(describe({ .ours = { 4, 4 }, .theirs = { 6, 8 } }, PeerRole::Daemon)
          == "the daemon speaks protocol 6-8, this client speaks 4-4");
    CHECK(
        describe({ .ours = { 4, 4 }, .theirs = { 3, 3 } }, PeerRole::Daemon)
        == "the daemon speaks protocol 3, from a release before version negotiation; this client speaks 4-4");
    CHECK(
        describe({ .ours = { 4, 4 }, .theirs = { 3, 3 } }, PeerRole::Client)
        == "the client speaks protocol 3, from a release before version negotiation; this daemon speaks 4-4");
    CHECK(describe({ .ours = { 4, 4 }, .theirs = { 6, 8 } }, PeerRole::Client)
          == "the client speaks protocol 6-8, this daemon speaks 4-4");
}

TEST_CASE("describe blames the token when the ranges overlap", "[vthost][negotiation]")
{
    CHECK(describe({ .ours = { 4, 4 }, .theirs = { 4, 9 } }, PeerRole::Daemon)
          == "the daemon refused the connection: wrong token or insufficient rights");
    CHECK(describe({ .ours = { 4, 4 }, .theirs = { 4, 9 } }, PeerRole::Client)
          == "the client was refused: wrong token or insufficient rights");
}

TEST_CASE("CapabilitySet keeps unknown idents and intersects", "[vthost][negotiation]")
{
    auto const theirs = CapabilitySet { .idents = { 1, 4711 } };
    CHECK(theirs.contains(Capability::ExtensionPermissions));
    CHECK(intersect(theirs, CapabilitySet::of({ Capability::ExtensionPermissions }))
          == CapabilitySet::of({ Capability::ExtensionPermissions }));
    CHECK(intersect(CapabilitySet { .idents = { 9, 1, 9, 1 } }, CapabilitySet { .idents = { 1, 9, 2 } })
          == CapabilitySet { .idents = { 1, 9 } });
    CHECK_FALSE(CapabilitySet {}.contains(Capability::ExtensionPermissions));
}

TEST_CASE("every Capability has exactly one table row", "[vthost][negotiation]")
{
    for (auto const& row: CapabilityTable)
    {
        CHECK(static_cast<uint16_t>(row.capability) != 0);
        CHECK_FALSE(row.name.empty());
        CHECK(std::ranges::count(CapabilityTable, row.capability, &CapabilityRow::capability) == 1);
        CHECK(std::ranges::count(CapabilityTable, row.name, &CapabilityRow::name) == 1);
    }
}

TEST_CASE("the move-node capability keeps its ident and name", "[vthost][negotiation][move]")
{
    STATIC_CHECK(std::to_underlying(Capability::MoveNode) == 16);
    auto const row = std::ranges::find(CapabilityTable, Capability::MoveNode, &CapabilityRow::capability);
    REQUIRE(row != CapabilityTable.end());
    CHECK(row->name == "move-node");
}

TEST_CASE("the layout-batch capability keeps its ident and name", "[vthost][negotiation][move]")
{
    STATIC_CHECK(std::to_underlying(Capability::LayoutBatch) == 17);
    auto const row = std::ranges::find(CapabilityTable, Capability::LayoutBatch, &CapabilityRow::capability);
    REQUIRE(row != CapabilityTable.end());
    CHECK(row->name == "layout-batch");
}

TEST_CASE("the live-upgrade capability keeps its ident and name", "[vthost][negotiation][upgrade]")
{
    STATIC_CHECK(std::to_underlying(Capability::LiveUpgrade) == 18);
    auto const row = std::ranges::find(CapabilityTable, Capability::LiveUpgrade, &CapabilityRow::capability);
    REQUIRE(row != CapabilityTable.end());
    CHECK(row->name == "live-upgrade");
}

TEST_CASE("the panes-reprojected capability keeps its ident and name", "[vthost][negotiation]")
{
    STATIC_CHECK(std::to_underlying(Capability::PanesReprojected) == 19);
    auto const row =
        std::ranges::find(CapabilityTable, Capability::PanesReprojected, &CapabilityRow::capability);
    REQUIRE(row != CapabilityTable.end());
    CHECK(row->name == "panes-reprojected");
}

TEST_CASE("the clipboard-targets capability keeps its ident and name", "[vthost][negotiation][clipboard]")
{
    STATIC_CHECK(std::to_underlying(Capability::ClipboardTargets) == 21);
    auto const row =
        std::ranges::find(CapabilityTable, Capability::ClipboardTargets, &CapabilityRow::capability);
    REQUIRE(row != CapabilityTable.end());
    CHECK(row->name == "clipboard-targets");
}

TEST_CASE("the cell-pixel-size capability keeps its ident and name", "[vthost][negotiation][cellsize]")
{
    STATIC_CHECK(std::to_underlying(Capability::CellPixelSize) == 20);
    auto const row =
        std::ranges::find(CapabilityTable, Capability::CellPixelSize, &CapabilityRow::capability);
    REQUIRE(row != CapabilityTable.end());
    CHECK(row->name == "cell-pixel-size");
}

TEST_CASE("the soft-fonts capability keeps its ident and name", "[vthost][negotiation][softfont]")
{
    STATIC_CHECK(std::to_underlying(Capability::SoftFonts) == 22);
    auto const row = std::ranges::find(CapabilityTable, Capability::SoftFonts, &CapabilityRow::capability);
    REQUIRE(row != CapabilityTable.end());
    CHECK(row->name == "soft-fonts");
}

TEST_CASE("DaemonFollow keeps its ident and name", "[vthost][negotiation][workspace-follow]")
{
    STATIC_CHECK(std::to_underlying(Capability::DaemonFollow) == 28);
    auto const follow =
        std::ranges::find(CapabilityTable, Capability::DaemonFollow, &CapabilityRow::capability);
    REQUIRE(follow != CapabilityTable.end());
    CHECK(follow->name == "daemon-follow");
}

TEST_CASE("capability 29 is workspace-meta", "[vthost][negotiation][workspace-meta]")
{
    STATIC_CHECK(std::to_underlying(Capability::WorkspaceMeta) == 29);
    auto const meta =
        std::ranges::find(CapabilityTable, Capability::WorkspaceMeta, &CapabilityRow::capability);
    REQUIRE(meta != CapabilityTable.end());
    CHECK(meta->name == "workspace-meta");
}

TEST_CASE("capability 30 is client-goodbye", "[vthost][negotiation][goodbye]")
{
    STATIC_CHECK(std::to_underlying(Capability::ClientGoodbye) == 30);
    auto const goodbye =
        std::ranges::find(CapabilityTable, Capability::ClientGoodbye, &CapabilityRow::capability);
    REQUIRE(goodbye != CapabilityTable.end());
    CHECK(goodbye->name == "client-goodbye");
}

TEST_CASE("capability 31 is launch-first-tab", "[vthost][negotiation][launch-first-tab]")
{
    STATIC_CHECK(std::to_underlying(Capability::LaunchFirstTab) == 31);
    auto const firstTab =
        std::ranges::find(CapabilityTable, Capability::LaunchFirstTab, &CapabilityRow::capability);
    REQUIRE(firstTab != CapabilityTable.end());
    CHECK(firstTab->name == "launch-first-tab");
}

TEST_CASE("capability 32 is names", "[vthost][negotiation][compat-0.9.0]")
{
    STATIC_CHECK(std::to_underlying(Capability::Names) == 32);
    auto const names = std::ranges::find(CapabilityTable, Capability::Names, &CapabilityRow::capability);
    REQUIRE(names != CapabilityTable.end());
    CHECK(names->name == "names");
}

TEST_CASE("capability 33 is workspace-color", "[vthost][negotiation][workspace-color]")
{
    STATIC_CHECK(std::to_underlying(Capability::WorkspaceColor) == 33);
    auto const color =
        std::ranges::find(CapabilityTable, Capability::WorkspaceColor, &CapabilityRow::capability);
    REQUIRE(color != CapabilityTable.end());
    CHECK(color->name == "workspace-color");
}

TEST_CASE("capability 35 is observe", "[vthost][negotiation][observer]")
{
    STATIC_CHECK(std::to_underlying(Capability::Observe) == 35);
    auto const observe = std::ranges::find(CapabilityTable, Capability::Observe, &CapabilityRow::capability);
    REQUIRE(observe != CapabilityTable.end());
    CHECK(observe->name == "observe");
}

TEST_CASE("observing needs observe, daemon-follow and workspace-meta shared",
          "[vthost][negotiation][observer]")
{
    auto const all =
        CapabilitySet::of({ Capability::Observe, Capability::DaemonFollow, Capability::WorkspaceMeta });
    CHECK(sharesObserving(all));
    for (auto const missing: ObserverCapabilities)
    {
        auto lacking = all;
        std::erase(lacking.idents, std::to_underlying(missing));
        CHECK_FALSE(sharesObserving(lacking));
    }
}

TEST_CASE("capability 34 is daemon-attention", "[vthost][negotiation][daemon-attention]")
{
    STATIC_CHECK(std::to_underlying(Capability::DaemonAttention) == 34);
    auto const attention =
        std::ranges::find(CapabilityTable, Capability::DaemonAttention, &CapabilityRow::capability);
    REQUIRE(attention != CapabilityTable.end());
    CHECK(attention->name == "daemon-attention");
}

TEST_CASE("the workspace switch capability keeps its ident and name",
          "[vthost][negotiation][workspace-follow]")
{
    STATIC_CHECK(std::to_underlying(Capability::WorkspaceSwitch) == 24);
    auto const change =
        std::ranges::find(CapabilityTable, Capability::WorkspaceSwitch, &CapabilityRow::capability);
    REQUIRE(change != CapabilityTable.end());
    CHECK(change->name == "workspace-switch");
}

TEST_CASE("ControlVersions spans every version since a control PDU last changed", "[vthost][negotiation]")
{
    CHECK(ControlVersions == VersionRange { .min = 4, .max = 5 });
    CHECK(negotiate(ControlVersions, { 4, 4 }) == 4u);
    CHECK(negotiate(ControlVersions, { 5, 5 }) == 5u);
    CHECK_FALSE(negotiate(ControlVersions, { 3, 3 }));
}

TEST_CASE("the control PDU table names each PDU once", "[vthost][negotiation]")
{
    auto types = std::ranges::to<std::vector>(std::views::transform(ControlPdus, &ControlPdu::type));
    std::ranges::sort(types);
    CHECK(std::ranges::adjacent_find(types) == types.end());
}
