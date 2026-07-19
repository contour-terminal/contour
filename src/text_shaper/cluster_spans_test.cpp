// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/cluster_spans.h>

#include <catch2/catch_test_macros.hpp>

using namespace text;

namespace
{

/// Builds the shaped-glyph view the segmenter consumes: one entry per output glyph, in visual order.
[[nodiscard]] std::vector<shaped_glyph_ref> glyphsOf(std::initializer_list<shaped_glyph_ref> glyphs)
{
    return std::vector<shaped_glyph_ref> { glyphs };
}

[[nodiscard]] std::vector<unsigned> clustersOf(std::initializer_list<unsigned> clusters)
{
    return std::vector<unsigned> { clusters };
}

} // namespace

TEST_CASE("cluster_spans.empty_run", "[cluster_spans]")
{
    auto const clusters = clustersOf({});
    auto const groups = clusterGroups({}, clusters);

    REQUIRE(groups.has_value());
    CHECK(groups->empty());
}

TEST_CASE("cluster_spans.one_to_one_all_covered", "[cluster_spans]")
{
    // Three cells, one codepoint and one glyph each, all rendered by the shaping font.
    auto const glyphs = glyphsOf({ { 0, false }, { 1, false }, { 2, false } });
    auto const clusters = clustersOf({ 0, 1, 2 });

    auto const groups = clusterGroups(glyphs, clusters);
    REQUIRE(groups.has_value());
    REQUIRE(groups->size() == 3);

    CHECK((*groups)[0]
          == cluster_group {
              .glyphBegin = 0, .glyphEnd = 1, .codepointBegin = 0, .codepointEnd = 1, .missing = false });
    CHECK((*groups)[1]
          == cluster_group {
              .glyphBegin = 1, .glyphEnd = 2, .codepointBegin = 1, .codepointEnd = 2, .missing = false });
    CHECK((*groups)[2]
          == cluster_group {
              .glyphBegin = 2, .glyphEnd = 3, .codepointBegin = 2, .codepointEnd = 3, .missing = false });
}

TEST_CASE("cluster_spans.missing_at_each_boundary", "[cluster_spans]")
{
    auto const clusters = clustersOf({ 0, 1, 2 });

    SECTION("missing at start")
    {
        auto const glyphs = glyphsOf({ { 0, true }, { 1, false }, { 2, false } });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());
        REQUIRE(groups->size() == 3);
        CHECK((*groups)[0].missing);
        CHECK_FALSE((*groups)[1].missing);
        CHECK_FALSE((*groups)[2].missing);
        CHECK((*groups)[0].codepointBegin == 0);
        CHECK((*groups)[0].codepointEnd == 1);
    }

    SECTION("missing in the middle")
    {
        auto const glyphs = glyphsOf({ { 0, false }, { 1, true }, { 2, false } });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());
        REQUIRE(groups->size() == 3);
        CHECK_FALSE((*groups)[0].missing);
        CHECK((*groups)[1].missing);
        CHECK_FALSE((*groups)[2].missing);
        CHECK((*groups)[1].codepointBegin == 1);
        CHECK((*groups)[1].codepointEnd == 2);
    }

    SECTION("missing at end")
    {
        auto const glyphs = glyphsOf({ { 0, false }, { 1, false }, { 2, true } });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());
        REQUIRE(groups->size() == 3);
        CHECK_FALSE((*groups)[0].missing);
        CHECK_FALSE((*groups)[1].missing);
        CHECK((*groups)[2].missing);
        CHECK((*groups)[2].codepointBegin == 2);
        CHECK((*groups)[2].codepointEnd == 3);
    }
}

TEST_CASE("cluster_spans.ligature_is_never_split", "[cluster_spans]")
{
    // The shaper merged the first two cells into one glyph, which carries the smaller cluster value.
    // The group must own BOTH codepoints, so that a fallback re-shapes the ligature whole.
    auto const glyphs = glyphsOf({ { 0, true }, { 2, false } });
    auto const clusters = clustersOf({ 0, 1, 2 });

    auto const groups = clusterGroups(glyphs, clusters);
    REQUIRE(groups.has_value());
    REQUIRE(groups->size() == 2);

    CHECK((*groups)[0].missing);
    CHECK((*groups)[0].codepointBegin == 0);
    CHECK((*groups)[0].codepointEnd == 2); // both codepoints of the ligature, not just the first
    CHECK((*groups)[1].codepointBegin == 2);
    CHECK((*groups)[1].codepointEnd == 3);
}

TEST_CASE("cluster_spans.combining_mark_is_never_torn_from_its_base", "[cluster_spans]")
{
    // One cell (cluster 3) holding a base plus two combining marks, shaped into two glyphs. The mark is
    // missing; the base is not. The whole cell must travel to the fallback font together.
    auto const glyphs = glyphsOf({ { 3, false }, { 3, true } });
    auto const clusters = clustersOf({ 3, 3, 3 });

    auto const groups = clusterGroups(glyphs, clusters);
    REQUIRE(groups.has_value());
    REQUIRE(groups->size() == 1);

    CHECK((*groups)[0].missing);
    CHECK((*groups)[0].glyphBegin == 0);
    CHECK((*groups)[0].glyphEnd == 2);
    CHECK((*groups)[0].codepointBegin == 0);
    CHECK((*groups)[0].codepointEnd == 3); // base and both marks
}

TEST_CASE("cluster_spans.clusters_are_cell_labels_not_codepoint_offsets", "[cluster_spans]")
{
    // Within a run, cluster values start at whatever cell the run begins at. They are opaque monotone
    // labels, and must be mapped back to codepoint indices rather than used as indices.
    auto const glyphs = glyphsOf({ { 5, false }, { 6, true }, { 7, false } });
    auto const clusters = clustersOf({ 5, 6, 7 });

    auto const groups = clusterGroups(glyphs, clusters);
    REQUIRE(groups.has_value());
    REQUIRE(groups->size() == 3);

    CHECK((*groups)[0].codepointBegin == 0);
    CHECK((*groups)[0].codepointEnd == 1);
    CHECK((*groups)[1].codepointBegin == 1);
    CHECK((*groups)[1].codepointEnd == 2);
    CHECK((*groups)[1].missing);
    CHECK((*groups)[2].codepointBegin == 2);
    CHECK((*groups)[2].codepointEnd == 3);
}

TEST_CASE("cluster_spans.multi_codepoint_cell", "[cluster_spans]")
{
    // Cell 0 holds two codepoints, cell 1 holds one.
    auto const glyphs = glyphsOf({ { 0, false }, { 1, true } });
    auto const clusters = clustersOf({ 0, 0, 1 });

    auto const groups = clusterGroups(glyphs, clusters);
    REQUIRE(groups.has_value());
    REQUIRE(groups->size() == 2);

    CHECK((*groups)[0].codepointBegin == 0);
    CHECK((*groups)[0].codepointEnd == 2); // the boundary lands past the whole cell
    CHECK((*groups)[1].codepointBegin == 2);
    CHECK((*groups)[1].codepointEnd == 3);
}

TEST_CASE("cluster_spans.dropped_glyphs_leave_no_codepoint_unowned", "[cluster_spans]")
{
    auto const clusters = clustersOf({ 0, 1, 2 });

    SECTION("leading glyph dropped by the shaper")
    {
        auto const glyphs = glyphsOf({ { 1, false }, { 2, false } });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());
        REQUIRE(groups->size() == 2);
        CHECK((*groups)[0].codepointBegin == 0); // claimed by the first group regardless
        CHECK((*groups)[0].codepointEnd == 2);
    }

    SECTION("trailing glyph dropped by the shaper")
    {
        auto const glyphs = glyphsOf({ { 0, false }, { 1, false } });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());
        REQUIRE(groups->size() == 2);
        CHECK((*groups)[1].codepointBegin == 1);
        CHECK((*groups)[1].codepointEnd == 3); // claimed by the last group regardless
    }
}

TEST_CASE("cluster_spans.non_monotone_input_is_rejected", "[cluster_spans]")
{
    SECTION("glyph clusters out of order")
    {
        auto const glyphs = glyphsOf({ { 2, false }, { 0, false } });
        auto const clusters = clustersOf({ 0, 1, 2 });
        CHECK_FALSE(clusterGroups(glyphs, clusters).has_value());
    }

    SECTION("input clusters out of order")
    {
        auto const glyphs = glyphsOf({ { 0, false } });
        auto const clusters = clustersOf({ 2, 1, 0 });
        CHECK_FALSE(clusterGroups(glyphs, clusters).has_value());
    }
}

TEST_CASE("cluster_spans.indivisibleGroup", "[cluster_spans]")
{
    SECTION("covers the whole run, and is missing if any glyph is")
    {
        auto const glyphs = glyphsOf({ { 0, false }, { 1, true }, { 2, false } });
        auto const group = indivisibleGroup(glyphs, 3);

        CHECK(group.glyphBegin == 0);
        CHECK(group.glyphEnd == 3);
        CHECK(group.codepointBegin == 0);
        CHECK(group.codepointEnd == 3);
        CHECK(group.missing);
    }

    SECTION("a fully covered run is not missing")
    {
        auto const glyphs = glyphsOf({ { 0, false }, { 1, false } });
        CHECK_FALSE(indivisibleGroup(glyphs, 2).missing);
    }

    SECTION("no glyphs at all")
    {
        auto const group = indivisibleGroup({}, 0);
        CHECK(group.empty());
        CHECK_FALSE(group.missing);
    }
}

TEST_CASE("cluster_spans.mergeAdjacentMissing", "[cluster_spans]")
{
    SECTION("adjacent missing groups become one fallback span")
    {
        // The issue #1939 shape: the ornament and the non-breaking space that follows it are adjacent
        // misses, and must be offered to a fallback font as a single span.
        auto const glyphs = glyphsOf({ { 0, true }, { 1, true }, { 2, false }, { 3, false } });
        auto const clusters = clustersOf({ 0, 1, 2, 3 });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());

        auto const segments = mergeAdjacentMissing(*groups);
        REQUIRE(segments.size() == 3);

        CHECK(segments[0].missing);
        CHECK(segments[0].glyphBegin == 0);
        CHECK(segments[0].glyphEnd == 2);
        CHECK(segments[0].codepointBegin == 0);
        CHECK(segments[0].codepointEnd == 2);
        CHECK_FALSE(segments[1].missing);
        CHECK_FALSE(segments[2].missing);
    }

    SECTION("disjoint missing groups stay separate")
    {
        auto const glyphs = glyphsOf({ { 0, true }, { 1, false }, { 2, true } });
        auto const clusters = clustersOf({ 0, 1, 2 });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());

        auto const segments = mergeAdjacentMissing(*groups);
        REQUIRE(segments.size() == 3);
        CHECK(segments[0].missing);
        CHECK_FALSE(segments[1].missing);
        CHECK(segments[2].missing);
    }

    SECTION("an entirely missing run becomes a single span")
    {
        auto const glyphs = glyphsOf({ { 0, true }, { 1, true }, { 2, true } });
        auto const clusters = clustersOf({ 0, 1, 2 });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());

        auto const segments = mergeAdjacentMissing(*groups);
        REQUIRE(segments.size() == 1);
        CHECK(segments[0].missing);
        CHECK(segments[0].codepointBegin == 0);
        CHECK(segments[0].codepointEnd == 3);
        CHECK(segments[0].glyphBegin == 0);
        CHECK(segments[0].glyphEnd == 3);
    }

    SECTION("a fully covered run passes through untouched")
    {
        auto const glyphs = glyphsOf({ { 0, false }, { 1, false } });
        auto const clusters = clustersOf({ 0, 1 });
        auto const groups = clusterGroups(glyphs, clusters);
        REQUIRE(groups.has_value());

        auto const segments = mergeAdjacentMissing(*groups);
        REQUIRE(segments.size() == 2);
        CHECK(segments == *groups);
    }

    SECTION("no groups")
    {
        CHECK(mergeAdjacentMissing({}).empty());
    }
}

TEST_CASE("cluster_spans.empty_group_detection", "[cluster_spans]")
{
    // Glyphs but no input codepoints: every group maps to an empty codepoint range, and the fallback
    // walker must never recurse on it.
    auto const glyphs = glyphsOf({ { 0, true } });
    auto const groups = clusterGroups(glyphs, {});

    REQUIRE(groups.has_value());
    REQUIRE(groups->size() == 1);
    CHECK((*groups)[0].empty());
}

// HarfBuzz emits a right-to-left run's glyphs in visual order, so their clusters DESCEND.
// clusterGroups() rejects an unsorted input outright, which drops the whole run to a single
// indivisible group -- losing per-cluster font fallback for exactly the scripts that need it most.
//
// open_shaper therefore normalises at the boundary: it reverses an RTL run's glyphs so that
// everything downstream sees ascending clusters and needs no direction awareness at all. These two
// cases pin both halves of that contract.
TEST_CASE("cluster_spans.descending_clusters_degrade", "[cluster_spans]")
{
    // What HarfBuzz hands back for an RTL run, unnormalised.
    auto const glyphs = glyphsOf({ { 2, false }, { 1, false }, { 0, false } });
    auto const clusters = clustersOf({ 2, 1, 0 });

    CHECK_FALSE(clusterGroups(glyphs, clusters).has_value());

    // ... and fallbackSegments() then yields one indivisible group covering everything, which is the
    // degradation the normalisation exists to avoid.
    auto const segments = fallbackSegments(glyphs, clusters);
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].glyphBegin == 0);
    CHECK(segments[0].glyphEnd == 3);
    CHECK(segments[0].codepointBegin == 0);
    CHECK(segments[0].codepointEnd == 3);
}

TEST_CASE("cluster_spans.reversed_rtl_run_segments_per_cluster", "[cluster_spans]")
{
    // The same run after open_shaper's reversal: ascending clusters, so it segments normally and
    // each cell can fall back to its own font.
    auto const glyphs = glyphsOf({ { 0, false }, { 1, false }, { 2, false } });
    auto const clusters = clustersOf({ 0, 1, 2 });

    auto const groups = clusterGroups(glyphs, clusters);
    REQUIRE(groups.has_value());
    CHECK(groups->size() == 3);

    // A missing glyph in the middle stays its own segment rather than swallowing the run.
    auto const withMissing = glyphsOf({ { 0, false }, { 1, true }, { 2, false } });
    auto const segments = fallbackSegments(withMissing, clusters);
    REQUIRE(segments.size() == 3);
    CHECK_FALSE(segments[0].missing);
    CHECK(segments[1].missing);
    CHECK_FALSE(segments[2].missing);
}
