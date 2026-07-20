// SPDX-License-Identifier: Apache-2.0
#include <contour/AsciiText.h>
#include <contour/FuzzyFilter.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace contour
{

namespace
{
    /// Sentinel for "this cell is not reachable" in the alignment table.
    constexpr auto Unreachable = std::numeric_limits<int>::min();

    /// True when @p candidate[index] starts a word: the first character, one after a separator, or a
    /// capital beginning a camel hump. This is what lets an acronym query ("tpz") find "Toggle Pane
    /// Zoom", and it is the single strongest ranking signal.
    ///
    /// Related to Command.cpp's startsWord(), but NOT the same predicate and deliberately not shared:
    /// that one splits a bare camel-case IDENTIFIER ("SplitVertical") and so must look ahead to keep an
    /// acronym run whole; this one scans rendered TEXT that already has separators in it ("Split
    /// Vertical"), where a separator is the boundary and no lookahead is needed.
    [[nodiscard]] constexpr bool isWordStart(std::string_view candidate, std::size_t index) noexcept
    {
        if (index == 0)
            return true;

        auto const previous = candidate[index - 1];
        if (!ascii::isWordCharacter(previous))
            return true;

        return ascii::isUpper(candidate[index]) && ascii::isLower(previous);
    }

    /// True when @p query is a subsequence of @p candidate at all, ignoring case.
    ///
    /// A greedy O(candidate) pre-check in front of the O(query x candidate) alignment below. Most
    /// candidates do not match at all — the palette scores ~90 commands against every keystroke, and a
    /// few characters typed rule nearly all of them out — so answering "no" here avoids allocating and
    /// filling the alignment table for them. Greedy is exact for mere EXISTENCE (taking the earliest
    /// occurrence of each character never loses a match); it is only for RANKING that the greedy answer
    /// is wrong, which is what the alignment below is for.
    [[nodiscard]] constexpr bool isSubsequence(std::string_view query, std::string_view candidate) noexcept
    {
        auto const* wanted = query.begin();
        for (auto const ch: candidate)
        {
            if (wanted == query.end())
                break;
            if (ascii::fold(ch) == ascii::fold(*wanted))
                ++wanted;
        }
        return wanted == query.end();
    }

    /// Fills the alignment table for @p query against @p candidate, and — when @p parents is non-null —
    /// the predecessor column that produced each cell, so callers that need the matched characters can
    /// backtrack. This is the single DP both fuzzyScore() and fuzzyMatch() share; keeping it in one
    /// place is what guarantees the score fuzzyMatch() reports is byte-for-byte the one fuzzyScore()
    /// ranks by.
    ///
    /// A full alignment rather than a greedy left-to-right scan. Greedy matching cannot rank. Preferring
    /// a word-start occurrence over an earlier mid-word one is what makes the ranking good ("spl" should
    /// find "Split Vertical", where it is a literal prefix, rather than "Toggle Split Orientation", where
    /// it only appears mid-title) — but a greedy scan that jumps ahead to a word start can jump PAST the
    /// only positions from which the rest of the query still fits, and would then report "no match" for a
    /// query that plainly matches. Concretely: typing the whole of "Toggle Status Line" with the spaces
    /// left out, the 'l' of "toggle" would bind to the 'L' of "Line", stranding the rest of the query
    /// with nothing left of the title to match against.
    ///
    /// So: score every way the query could align to the candidate and keep the best. best[i][j] is the
    /// score of the best alignment of query[0..i] in which query[i] lands on candidate[j].
    ///
    /// Cost is O(query * candidate) — a few thousand operations against a ~40-character title, and only
    /// for the candidates that survive the subsequence pre-check. It is not on any render path.
    ///
    /// @param query     Precondition: non-empty and a subsequence of @p candidate (both checked by the
    ///                  callers), and no longer than @p candidate.
    /// @param candidate The text being matched against.
    /// @param weights   How to score the match.
    /// @param best      Filled with the alignment scores; must be pre-sized query*candidate to Unreachable.
    /// @param parents   When non-null, filled (same size) with each cell's predecessor column, or -1.
    void computeAlignment(std::string_view query,
                          std::string_view candidate,
                          FuzzyWeights const& weights,
                          std::vector<int>& best,
                          std::vector<int>* parents)
    {
        auto const queryLength = query.size();
        auto const candidateLength = candidate.size();

        for (auto const i: std::views::iota(std::size_t { 0 }, queryLength))
        {
            auto const wanted = ascii::fold(query[i]);

            // Running best over all predecessors j' < j of (best[i-1][j'] - gap * j'), plus the column
            // that achieves it so a backtrack can recover the matched characters.
            //
            // The gap penalty is linear, so max over j' of (best[i-1][j'] + gap * (j - j' - 1)) factors
            // into (max over j' of (best[i-1][j'] - gap * j')) + gap * (j - 1). Carrying that maximum
            // forward turns the inner search over predecessors into O(1), which is what keeps the whole
            // thing quadratic rather than cubic.
            auto reachBest = Unreachable;
            auto reachArg = -1;

            for (auto const j: std::views::iota(std::size_t { 0 }, candidateLength))
            {
                // Fold predecessor j-1 into the running maximum BEFORE using it for j, so it covers
                // exactly the j' < j the recurrence quantifies over.
                if (i > 0 && j > 0)
                {
                    auto const previous = best[((i - 1) * candidateLength) + (j - 1)];
                    if (previous != Unreachable)
                    {
                        auto const reach = previous - (weights.gap * static_cast<int>(j - 1));
                        if (reachBest == Unreachable || reach > reachBest)
                        {
                            reachBest = reach;
                            reachArg = static_cast<int>(j - 1);
                        }
                    }
                }

                if (ascii::fold(candidate[j]) != wanted)
                    continue;

                auto score = 0;
                auto parent = -1;

                if (i == 0)
                {
                    // A match deep inside the candidate is a weaker one than a match up front, but the
                    // penalty is capped: past a handful of characters "further in" stops carrying
                    // information, and an uncapped penalty would just punish long titles.
                    auto const skipped = std::min(j, static_cast<std::size_t>(weights.leadingGapMax));
                    score = weights.leadingGap * static_cast<int>(skipped);
                }
                else
                {
                    if (reachBest == Unreachable)
                        continue; // query[i-1] cannot land anywhere before j: this cell is unreachable

                    score = reachBest + (weights.gap * static_cast<int>(j - 1));
                    parent = reachArg;

                    // Landing immediately after the previous match is worth more than any gapped
                    // alternative of equal reach, so it is scored separately rather than folded into the
                    // linear gap term.
                    if (j > 0)
                    {
                        auto const adjacent = best[((i - 1) * candidateLength) + (j - 1)];
                        if (adjacent != Unreachable && adjacent + weights.consecutive > score)
                        {
                            score = adjacent + weights.consecutive;
                            parent = static_cast<int>(j - 1);
                        }
                    }
                }

                if (isWordStart(candidate, j))
                    score += weights.wordStart;

                best[(i * candidateLength) + j] = score;
                if (parents != nullptr)
                    (*parents)[(i * candidateLength) + j] = parent;
            }
        }
    }
} // namespace

std::optional<int> fuzzyScore(std::string_view query, std::string_view candidate, FuzzyWeights const& weights)
{
    if (query.empty())
        return 0;

    auto const queryLength = query.size();
    auto const candidateLength = candidate.size();

    if (queryLength > candidateLength || !isSubsequence(query, candidate))
        return std::nullopt;

    auto best = std::vector<int>(queryLength * candidateLength, Unreachable);
    computeAlignment(query, candidate, weights, best, nullptr);

    // The best place for the query's LAST character to land decides the alignment.
    auto const lastRow = std::span(best).subspan((queryLength - 1) * candidateLength, candidateLength);

    // The pre-check already established that the query IS a subsequence, so some alignment exists.
    return std::ranges::max(lastRow);
}

std::optional<FuzzyMatch> fuzzyMatch(std::string_view query,
                                     std::string_view candidate,
                                     FuzzyWeights const& weights)
{
    if (query.empty())
        return FuzzyMatch { .score = 0, .positions = {} };

    auto const queryLength = query.size();
    auto const candidateLength = candidate.size();

    if (queryLength > candidateLength || !isSubsequence(query, candidate))
        return std::nullopt;

    auto best = std::vector<int>(queryLength * candidateLength, Unreachable);
    auto parents = std::vector<int>(queryLength * candidateLength, -1);
    computeAlignment(query, candidate, weights, best, &parents);

    // The column the query's last character lands on decides the whole alignment; take the best-scoring
    // one, the same "best cell of the last row" fuzzyScore ranks by. max_element keeps the FIRST of equal
    // maxima, i.e. the earliest column on a tie (Unreachable is INT_MIN, so it never wins).
    auto const lastRow = std::span(best).subspan((queryLength - 1) * candidateLength, candidateLength);
    auto const bestCell = std::ranges::max_element(lastRow);
    auto const bestScore = *bestCell;
    auto const lastColumn = static_cast<std::size_t>(bestCell - lastRow.begin());

    // Walk the predecessor chain from that column back to the first query character. Each parent is a
    // strictly smaller column (see computeAlignment), so the positions come out ascending.
    auto positions = std::vector<int>(queryLength);
    auto column = static_cast<int>(lastColumn);
    for (auto i = static_cast<int>(queryLength) - 1; i >= 0; --i)
    {
        positions[static_cast<std::size_t>(i)] = column;
        if (i > 0)
            column =
                parents[(static_cast<std::size_t>(i) * candidateLength) + static_cast<std::size_t>(column)];
    }

    return FuzzyMatch { .score = bestScore, .positions = std::move(positions) };
}

} // namespace contour
