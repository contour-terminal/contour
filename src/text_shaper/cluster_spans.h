// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace text
{

/// A shaped glyph reduced to what font fallback needs in order to decide about it.
struct shaped_glyph_ref
{
    unsigned cluster {}; ///< The input cluster the shaper assigned to this glyph.
    bool missing {};     ///< Whether the shaping font had no glyph for it (glyph index 0, .notdef).
};

/// A maximal group of consecutive shaped glyphs sharing one cluster value, together with the half-open
/// range of input codepoints that produced them.
struct cluster_group
{
    std::size_t glyphBegin {};     ///< First glyph index, into the shaped run.
    std::size_t glyphEnd {};       ///< One past the last glyph index.
    std::size_t codepointBegin {}; ///< First input codepoint index, into the run's codepoints.
    std::size_t codepointEnd {};   ///< One past the last input codepoint index.
    bool missing {};               ///< Whether any glyph in this group is .notdef.

    /// @return Whether this group maps to no input codepoints at all.
    [[nodiscard]] constexpr bool empty() const noexcept { return codepointBegin >= codepointEnd; }

    [[nodiscard]] constexpr bool operator==(cluster_group const&) const noexcept = default;
};

/// Segments a shaped run into cluster groups, mapping each group back to the input codepoints that
/// produced it.
///
/// This is the cluster-alignment step of font fallback. A glyph the shaping font could not render must
/// never be re-shaped in isolation: the shaper may have merged several input clusters into a single
/// glyph (a ligature), or emitted several glyphs for a single cluster (a base plus its combining marks).
/// Only a whole cluster group may be handed to another font.
///
/// Relies on the shaper's monotone-cluster guarantee: output glyph clusters are non-decreasing, glyphs
/// of one cluster are contiguous, and a merged cluster carries the smallest of the input cluster values
/// it was built from. A group's codepoint range is therefore
/// [first input index with cluster >= c(k), first input index with cluster >= c(k+1)), which covers a
/// merged ligature in full and never splits a cluster.
///
/// @param glyphs        The shaped glyphs, in visual (left-to-right) order.
/// @param inputClusters The per-codepoint cluster values handed to the shaper. These are cell indices,
///                      not codepoint offsets: they need not start at zero, and all codepoints of one
///                      cell share a value. Must be non-decreasing.
/// @return The cluster groups covering the whole run, or std::nullopt if either input violates the
///         monotone-cluster guarantee, in which case the caller must treat the run as indivisible.
[[nodiscard]] inline std::optional<std::vector<cluster_group>> clusterGroups(
    std::span<shaped_glyph_ref const> glyphs, std::span<unsigned const> inputClusters)
{
    if (!std::ranges::is_sorted(inputClusters))
        return std::nullopt;

    auto groups = std::vector<cluster_group> {};
    if (glyphs.empty())
        return groups;

    groups.reserve(glyphs.size());

    // Both sequences are non-decreasing, so one forward walk over the codepoints serves every group: the
    // codepoints a group owns end exactly where the next group's cluster begins.
    auto codepointCursor = std::size_t { 0 };
    auto const advanceTo = [&codepointCursor, inputClusters](unsigned cluster) noexcept {
        while (codepointCursor < inputClusters.size() && inputClusters[codepointCursor] < cluster)
            ++codepointCursor;
        return codepointCursor;
    };

    for (auto glyphIndex = std::size_t { 0 }; glyphIndex < glyphs.size();)
    {
        auto const cluster = glyphs[glyphIndex].cluster;

        // Groups are maximal runs of equal cluster, so each new group's cluster must strictly exceed the
        // previous one. Anything else means the run is not monotone and must not be split.
        if (!groups.empty() && cluster <= glyphs[groups.back().glyphBegin].cluster)
            return std::nullopt;

        auto glyphEnd = glyphIndex;
        auto missing = false;
        while (glyphEnd < glyphs.size() && glyphs[glyphEnd].cluster == cluster)
        {
            missing = missing || glyphs[glyphEnd].missing;
            ++glyphEnd;
        }

        auto const codepointBegin = advanceTo(cluster);
        auto const codepointEnd =
            glyphEnd < glyphs.size() ? advanceTo(glyphs[glyphEnd].cluster) : inputClusters.size();

        groups.emplace_back(cluster_group { .glyphBegin = glyphIndex,
                                            .glyphEnd = glyphEnd,
                                            .codepointBegin = codepointBegin,
                                            .codepointEnd = codepointEnd,
                                            .missing = missing });
        glyphIndex = glyphEnd;
    }

    // The run's codepoints are owned end to end, even where the shaper dropped a leading or trailing
    // codepoint outright (a default-ignorable, say) and no glyph claims it.
    groups.front().codepointBegin = 0;
    groups.back().codepointEnd = inputClusters.size();

    return groups;
}

/// @return The whole run as a single, indivisible group.
///
/// The conservative answer for when clusterGroups() has refused to segment a run: with no trustworthy
/// mapping from glyphs back to codepoints, the run may not be split, and a fallback font must be offered
/// all of it at once -- which is what font fallback did for every run before it learned to split them.
///
/// @param glyphs         The shaped glyphs of the run.
/// @param codepointCount The number of input codepoints the run was shaped from.
[[nodiscard]] inline cluster_group indivisibleGroup(std::span<shaped_glyph_ref const> glyphs,
                                                    std::size_t codepointCount)
{
    return cluster_group { .glyphBegin = 0,
                           .glyphEnd = glyphs.size(),
                           .codepointBegin = 0,
                           .codepointEnd = codepointCount,
                           .missing = std::ranges::any_of(
                               glyphs, [](shaped_glyph_ref const& ref) { return ref.missing; }) };
}

/// Coalesces neighbouring uncovered groups into single fallback segments.
///
/// Adjacent codepoints that the shaping font is missing belong to one fallback lookup, not several: the
/// prompt ornament and the non-breaking space that follows it must be offered to a fallback font
/// together, so that one font can answer for both. Covered groups pass through untouched, and the
/// returned segments still cover the run end to end, in order.
///
/// @param groups The cluster groups of one shaped run, as returned by clusterGroups().
/// @return The same run, with runs of missing groups merged.
[[nodiscard]] inline std::vector<cluster_group> mergeAdjacentMissing(std::span<cluster_group const> groups)
{
    auto segments = std::vector<cluster_group> {};
    segments.reserve(groups.size());

    for (auto index = std::size_t { 0 }; index < groups.size();)
    {
        if (!groups[index].missing)
        {
            segments.emplace_back(groups[index]);
            ++index;
            continue;
        }

        auto last = index;
        while (last + 1 < groups.size() && groups[last + 1].missing)
            ++last;

        segments.emplace_back(cluster_group { .glyphBegin = groups[index].glyphBegin,
                                              .glyphEnd = groups[last].glyphEnd,
                                              .codepointBegin = groups[index].codepointBegin,
                                              .codepointEnd = groups[last].codepointEnd,
                                              .missing = true });
        index = last + 1;
    }

    return segments;
}

/// Divides a shaped run into the segments font fallback works on.
///
/// Covered segments are kept as they are; each maximal stretch the shaping font could not render becomes
/// one segment, to be offered to a fallback font whole. A run whose clusters cannot be trusted to be
/// monotone yields a single indivisible segment -- the conservative answer, and what every run used to
/// get before fallback learned to split them.
///
/// @param glyphs   The shaped glyphs of the run, in visual (left-to-right) order.
/// @param clusters The per-codepoint cluster values the run was shaped with.
/// @return The segments, in order, covering the run end to end.
[[nodiscard]] inline std::vector<cluster_group> fallbackSegments(std::span<shaped_glyph_ref const> glyphs,
                                                                 std::span<unsigned const> clusters)
{
    if (auto const groups = clusterGroups(glyphs, clusters))
        return mergeAdjacentMissing(*groups);

    return { indivisibleGroup(glyphs, clusters.size()) };
}

} // namespace text
