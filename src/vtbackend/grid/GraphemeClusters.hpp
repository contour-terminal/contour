// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/LineSoA.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// One grapheme cluster of a text, and the columns the grid lays it out in.
struct MeasuredCluster
{
    std::string text;    ///< The cluster, UTF-8.
    ColumnCount columns; ///< The columns the grid gives it; at least one.
};

/// The columns the grid gives one grapheme cluster: at least one, as wide as its first codepoint, or, under
/// ClusterWidthPolicy::ClusterAware, as the whole cluster where that is wider -- a cluster widens and never
/// narrows (appendCodepointToCluster, Screen::applyClusterWidthChange).
/// @param cluster The cluster's codepoints; not empty.
/// @param policy  How the screen measures a cluster (DEC mode 2027).
/// @return Its columns.
[[nodiscard]] ColumnCount clusterColumns(std::u32string_view cluster, ClusterWidthPolicy policy) noexcept;

/// @p text cut into its grapheme clusters, each measured as the grid lays it out.
///
/// The one walk over clusters that text measured for the grid and text cut to fit it share, so a cut never
/// splits a cluster -- a base from its combining mark, a ZWJ sequence, a flag's pair -- and a fit is judged
/// by the columns the screen will actually take.
/// @param text   UTF-8 text.
/// @param policy How the screen measures a cluster (DEC mode 2027).
/// @return The clusters, in order; together they are @p text.
[[nodiscard]] std::vector<MeasuredCluster> graphemeClustersOf(std::string_view text,
                                                              ClusterWidthPolicy policy);

/// The columns @p text takes when a screen writes it.
/// @param text   UTF-8 text.
/// @param policy How the screen measures a cluster (DEC mode 2027).
/// @return The sum of its clusters' columns.
[[nodiscard]] ColumnCount columnsOnTheGrid(std::string_view text, ClusterWidthPolicy policy);

} // namespace vtbackend
