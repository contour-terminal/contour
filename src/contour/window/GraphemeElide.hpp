// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Eliding text by grapheme clusters, never inside one: a base with its combining marks or an emoji ZWJ
/// sequence is one piece. The budget is counted in clusters or in terminal cells. One implementation for
/// everything in the application that shortens a string to fit: the location chip, the link pill, a program's
/// status badge.

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>
#include <libunicode/width.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::window
{

/// What a budget counts.
enum class ElideUnit : uint8_t
{
    Clusters, ///< One per grapheme cluster, however wide it draws.
    Cells,    ///< The terminal cells a cluster takes (unicode::grapheme_cluster_width).
};

/// The text an elided string carries in place of what it dropped.
inline constexpr auto ElideMarker = std::string_view { "…" };

/// One grapheme cluster of a text: its UTF-8 and the terminal cells it takes.
struct Cluster
{
    std::string text;  ///< The cluster, UTF-8.
    std::size_t cells; ///< Its width in cells (unicode::grapheme_cluster_width).
};

/// @param text UTF-8 text.
/// @return The text's grapheme clusters in order, so an emoji ZWJ sequence or a base with its combining
///         marks is one piece that is never split.
[[nodiscard]] inline std::vector<Cluster> clustersOf(std::string_view text)
{
    auto clusters = std::vector<Cluster> {};
    for (std::u32string const& cluster: unicode::utf8_grapheme_segmenter(text))
        clusters.push_back(Cluster { .text = unicode::convert_to<char>(std::u32string_view { cluster }),
                                     .cells = unicode::grapheme_cluster_width(cluster) });
    return clusters;
}

/// @param cluster A cluster.
/// @param unit    What a budget counts.
/// @return What @p cluster takes of a budget in @p unit.
[[nodiscard]] constexpr std::size_t unitsOf(Cluster const& cluster, ElideUnit unit) noexcept
{
    return unit == ElideUnit::Cells ? cluster.cells : 1;
}

/// @param clusters A text's clusters.
/// @param unit     What a budget counts.
/// @return What they take together.
[[nodiscard]] constexpr std::size_t unitsOf(std::span<Cluster const> clusters, ElideUnit unit) noexcept
{
    return std::ranges::fold_left(
        clusters | std::views::transform([unit](Cluster const& cluster) { return unitsOf(cluster, unit); }),
        std::size_t { 0 },
        std::plus<> {});
}

/// @param clusters A text's clusters, from the end the piece is taken from.
/// @param budget   The most a piece may take.
/// @param unit     What the budget counts.
/// @return How many clusters from the front of @p clusters fit in @p budget.
[[nodiscard]] constexpr std::size_t countWithin(std::span<Cluster const> clusters,
                                                std::size_t budget,
                                                ElideUnit unit) noexcept
{
    auto used = std::size_t { 0 };
    auto count = std::size_t { 0 };
    for (auto const& cluster: clusters)
    {
        if (used + unitsOf(cluster, unit) > budget)
            break;
        used += unitsOf(cluster, unit);
        ++count;
    }
    return count;
}

/// @param clusters A text's clusters.
/// @return The clusters' text joined.
[[nodiscard]] inline std::string joined(std::span<Cluster const> clusters)
{
    auto text = std::string {};
    for (auto const& cluster: clusters)
        text += cluster.text;
    return text;
}

/// @param text     The text.
/// @param clusters @p text's grapheme clusters (clustersOf()), for a caller that elides it more than once.
/// @param maxUnits The most the result may take, the marker counted as one.
/// @param unit     What @p maxUnits counts.
/// @return @p text if it fits, else its head, the marker and its tail, whole clusters each; the head keeps
/// the
///         odd one, so what leads reads first. A budget of one or none leaves the marker alone.
[[nodiscard]] inline std::string elideMiddle(std::string_view text,
                                             std::span<Cluster const> clusters,
                                             std::size_t maxUnits,
                                             ElideUnit unit)
{
    if (unitsOf(clusters, unit) <= maxUnits)
        return std::string { text };
    if (maxUnits <= 1)
        return std::string { ElideMarker };
    auto const keep = maxUnits - 1; // The marker takes one.
    auto const head = countWithin(clusters, (keep + 1) / 2, unit);
    auto const headUnits = unitsOf(clusters.first(head), unit);
    // The tail is what fits from the back, in what the head left of the budget.
    auto const reversed = std::vector<Cluster>(clusters.rbegin(), clusters.rend());
    auto const tail = countWithin(reversed, keep - headUnits, unit);
    return joined(clusters.first(head)) + std::string { ElideMarker } + joined(clusters.last(tail));
}

/// @param text     UTF-8 text.
/// @param maxUnits The most the result may take, the marker counted as one.
/// @param unit     What @p maxUnits counts.
/// @return elideMiddle() of @p text's clusters.
[[nodiscard]] inline std::string elideMiddle(std::string_view text, std::size_t maxUnits, ElideUnit unit)
{
    return elideMiddle(text, clustersOf(text), maxUnits, unit);
}

/// @param text     UTF-8 text.
/// @param maxUnits The most the result may take, the marker counted as one.
/// @param unit     What @p maxUnits counts.
/// @return @p text if it fits, else its leading whole clusters and the marker; a budget of one or none leaves
///         the marker alone.
[[nodiscard]] inline std::string elideEnd(std::string_view text, std::size_t maxUnits, ElideUnit unit)
{
    auto const clusters = clustersOf(text);
    if (unitsOf(clusters, unit) <= maxUnits)
        return std::string { text };
    if (maxUnits <= 1)
        return std::string { ElideMarker };
    auto const head = countWithin(clusters, maxUnits - 1, unit);
    return joined(std::span(clusters).first(head)) + std::string { ElideMarker };
}

} // namespace contour::window
