// SPDX-License-Identifier: Apache-2.0
//
// The command palette's filter. What matters is not just "does it match" but "does the thing the user
// meant come up FIRST" — a palette that finds the right command and ranks it fourth has failed.

#include <contour/FuzzyFilter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace contour;

namespace
{
/// Ranks @p candidates against @p query the way the palette does (best first), dropping non-matches.
[[nodiscard]] std::vector<std::string> ranked(std::string_view query,
                                              std::vector<std::string> const& candidates)
{
    struct Scored
    {
        std::string text;
        int score;
    };

    auto scored = std::vector<Scored> {};
    for (auto const& candidate: candidates)
        if (auto const score = fuzzyScore(query, candidate))
            scored.push_back(Scored { .text = candidate, .score = *score });

    std::ranges::stable_sort(scored, [](auto const& a, auto const& b) { return a.score > b.score; });

    auto result = std::vector<std::string> {};
    for (auto const& entry: scored)
        result.push_back(entry.text);
    return result;
}
} // namespace

TEST_CASE("fuzzyScore matches a subsequence, not just a prefix", "[contour][palette]")
{
    SECTION("a contiguous substring matches")
    {
        REQUIRE(fuzzyScore("split", "Split Vertical").has_value());
    }

    SECTION("a gapped subsequence matches — this is what makes it fuzzy")
    {
        // The user typing initials is the single most common palette gesture.
        REQUIRE(fuzzyScore("sv", "Split Vertical").has_value());
        REQUIRE(fuzzyScore("tpz", "Toggle Pane Zoom").has_value());
    }

    SECTION("matching is case-insensitive in both directions")
    {
        REQUIRE(fuzzyScore("SPLIT", "Split Vertical").has_value());
        REQUIRE(fuzzyScore("split", "SPLIT VERTICAL").has_value());
    }

    SECTION("characters must appear IN ORDER")
    {
        // "vs" is not "sv": a fuzzy matcher that ignored order would match essentially everything and
        // the filter would be useless.
        CHECK_FALSE(fuzzyScore("vs", "Split Vertical").has_value());
    }

    SECTION("a character that is not there does not match")
    {
        CHECK_FALSE(fuzzyScore("splitz", "Split Vertical").has_value());
        CHECK_FALSE(fuzzyScore("quit", "Split Vertical").has_value());
    }

    SECTION("an empty query matches everything, unranked")
    {
        auto const score = fuzzyScore("", "Split Vertical");
        REQUIRE(score.has_value());
        CHECK(*score == 0);
    }

    SECTION("preferring a word start must never cost a match that exists")
    {
        // Regression. A greedy matcher that jumps ahead to a word-start occurrence can jump PAST the
        // only positions from which the rest of the query still fits, and then wrongly report no match.
        //
        // Here the 'l' of "toggle" would greedily bind to the 'L' of "Line" (a word start, and so
        // "better"), stranding "...statusline" with nothing left of the candidate to match against —
        // even though the query is plainly a subsequence of it.
        REQUIRE(fuzzyScore("togglestatusline", "Toggle Status Line").has_value());

        // The same shape, minimized: the 'a' could greedily take the word-start 'A' of the last word,
        // leaving no 'b' after it.
        REQUIRE(fuzzyScore("ab", "ab Ax").has_value());
    }
}

TEST_CASE("fuzzyScore ranks the command the user meant first", "[contour][palette]")
{
    SECTION("a word-start match beats a mid-word one")
    {
        // Typing "spl" means "Split ...". "Toggle Split Orientation" contains the same letters, but the
        // user reaching for it would have typed "tso" or "split o".
        auto const order = ranked("spl", { "Toggle Split Orientation", "Split Vertical" });
        REQUIRE(order.size() == 2);
        CHECK(order.front() == "Split Vertical");
    }

    SECTION("an acronym match finds the multi-word command it stands for")
    {
        auto const order = ranked("tpz", { "Toggle Pane Zoom", "Toggle Status Line" });
        REQUIRE(order.size() == 1);
        CHECK(order.front() == "Toggle Pane Zoom");
    }

    SECTION("an earlier match beats a later one, all else equal")
    {
        auto const order = ranked("tab", { "Switch To Previous Tab", "Tab Something" });
        REQUIRE(order.size() == 2);
        CHECK(order.front() == "Tab Something");
    }

    SECTION("a literal substring beats a scattered match, however many word starts it hits")
    {
        // "close" is a literal substring of "Close Tab" — the user typed the title. The other candidate
        // also contains c-l-o-s-e as a subsequence, and every one of those letters happens to land on a
        // capital, so a scoring scheme that valued word starts above contiguity would rank the long,
        // irrelevant command first. Contiguity is the stronger statement of intent.
        auto const order = ranked("close", { "Cancel Long Selection Everywhere", "Close Tab" });
        REQUIRE(order.size() == 2);
        CHECK(order.front() == "Close Tab");
    }
}
