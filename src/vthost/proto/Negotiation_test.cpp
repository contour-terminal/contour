// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

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
