// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string_view>

namespace contour
{

/// Scoring weights for fuzzyScore(), as data rather than as constants buried in the algorithm — so
/// the ranking can be re-tuned (or a caller can rank differently) without touching the matcher.
///
/// `consecutive` is deliberately as large as `wordStart`. A contiguous run means the user typed a
/// literal substring of the title, which is at least as strong a statement of intent as landing on a
/// word boundary — and if it is scored lower, a long title with many word starts can out-score a short
/// title the query matches outright ("close" would rank "Cancel Long Selection Everywhere" above
/// "Close Tab", since each of its scattered hits lands on a capital).
struct FuzzyWeights
{
    int wordStart = 15;    ///< Matched at the start of a word (the S and V "sv" finds in "Split Vertical").
    int consecutive = 15;  ///< Matched right after the previous match (the run "spl" makes in "Split").
    int leadingGap = -2;   ///< Per candidate character skipped before the FIRST match; capped below.
    int leadingGapMax = 6; ///< Most characters the leading-gap penalty is charged for.
    int gap = -1;          ///< Per candidate character skipped between two matches.
};

/// Scores how well @p query fuzzy-matches @p candidate, case-insensitively.
///
/// The match is a subsequence match — every character of @p query must occur in @p candidate, in
/// order, but not necessarily adjacently. That is what lets "sv" find "Split Vertical" and "tpz"
/// find "Toggle Pane Zoom".
///
/// The BEST alignment is scored, not merely the first one found: word-start hits and contiguous runs
/// are rewarded, so "spl" scores "Split Vertical" (where it is a literal prefix) above "Toggle Split
/// Orientation" (where it only appears mid-title). Finding the best alignment rather than greedily
/// taking each earliest-or-word-start occurrence is what keeps the two goals — matching at all, and
/// ranking well — from fighting each other; see the recurrence in the implementation.
///
/// Scores are only comparable within one @p query. An empty @p query matches everything with a score
/// of 0 (the palette shows the full list, unranked).
///
/// @param query     What the user typed.
/// @param candidate The text to match against (a command's title, or its id).
/// @param weights   How to score the match.
/// @return The score (higher is better), or nullopt when @p query is not a subsequence of @p candidate.
[[nodiscard]] std::optional<int> fuzzyScore(std::string_view query,
                                            std::string_view candidate,
                                            FuzzyWeights const& weights = {});

} // namespace contour
