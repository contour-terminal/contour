// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Search.hpp>

#include <crispy/Assert.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace contour::session
{

/// What the find bar's summary is saying about the current search.
///
/// Separate from the summary text because it drives appearance rather than wording: the field's edge
/// and the count turn red on @c NoMatch. Deriving that from the text (by comparing against "No
/// results") would break the moment the string is translated.
enum class SearchOutcome : uint8_t
{
    Idle = 0, //!< Nothing is being searched for. The bar shows no count at all.
    NoMatch,  //!< A pattern is active and matches nothing.
    Matched,  //!< At least one match exists.
};

/// Whether stepping to another match would actually go anywhere.
///
/// Not simply "are there matches": sitting on the only one, previous and next have nowhere to move,
/// and a live-looking button that does nothing is worse than an inert one.
enum class MatchNavigation : uint8_t
{
    Unavailable = 0,
    Available,
};

/// Everything the find bar derives from the terminal's search state.
struct SearchStatus
{
    std::string summary;
    SearchOutcome outcome = SearchOutcome::Idle;
    MatchNavigation navigation = MatchNavigation::Unavailable;
};

/// Whether the user has pinned the case policy, or left it to decide for itself.
///
/// Drives the toggle's lit state: lit says "you chose this", unlit says "smart case is deciding".
enum class CasePinned : uint8_t
{
    No = 0,
    Yes,
};

/// What the find bar's case affordance shows for one policy.
struct SearchCaseAffordance
{
    /// The glyph on the button. Its own case IS the state it names -- "Aa" for a comparison that
    /// respects case, "aa" for one that ignores it -- so the button reads without its tooltip.
    std::string_view glyph;
    std::string_view tooltip;
    CasePinned pinned = CasePinned::No;
};

/// How @p mode presents itself on the toggle.
///
/// A switch rather than arithmetic on the enumerator values, and here rather than in QML, because
/// this is exactly the mapping that went wrong when the bar spoke raw integers: Insensitive is 1 and
/// Sensitive is 2, so "the higher one is the strict one" is false, and every comparison built on that
/// assumption inverted the meaning without any test being able to notice.
[[nodiscard]] inline SearchCaseAffordance describeSearchCase(vtbackend::SearchCaseSensitivity mode)
{
    switch (mode)
    {
        case vtbackend::SearchCaseSensitivity::Smart:
            return { .glyph = "Aa",
                     .tooltip = "Match case: smart — exact only when the term has a capital",
                     .pinned = CasePinned::No };
        case vtbackend::SearchCaseSensitivity::Sensitive:
            return { .glyph = "Aa", .tooltip = "Match case: on", .pinned = CasePinned::Yes };
        case vtbackend::SearchCaseSensitivity::Insensitive:
            return { .glyph = "aa", .tooltip = "Match case: off", .pinned = CasePinned::Yes };
    }
    crispy::unreachable();
}

/// The policy the toggle moves to from @p mode.
///
/// Smart -> Sensitive -> Insensitive -> Smart. The two pinned modes come first because someone
/// reaching for this button has just decided smart case guessed wrong, and "on" is the commoner of
/// the two corrections. A switch, so a fourth policy is a compile error here rather than a silent
/// wrap -- which is what `(mode + 1) % 3` in QML would have been.
[[nodiscard]] inline vtbackend::SearchCaseSensitivity nextSearchCase(vtbackend::SearchCaseSensitivity mode)
{
    switch (mode)
    {
        case vtbackend::SearchCaseSensitivity::Smart: return vtbackend::SearchCaseSensitivity::Sensitive;
        case vtbackend::SearchCaseSensitivity::Sensitive:
            return vtbackend::SearchCaseSensitivity::Insensitive;
        case vtbackend::SearchCaseSensitivity::Insensitive: return vtbackend::SearchCaseSensitivity::Smart;
    }
    crispy::unreachable();
}

/// Derives what the find bar shows from the active pattern and its tally.
///
/// Pure, so the wording and the enabled/disabled rules are testable without a terminal, a window or
/// a Qt event loop -- which is the whole reason this is a header and not a method on TerminalSession.
///
/// @param pattern What is being searched for. Empty means no search is active.
/// @param tally   The match tally for @p pattern. @see vtbackend::Terminal::tallySearchMatches.
/// @return The summary text, why it says that, and whether stepping is possible.
[[nodiscard]] inline SearchStatus describeSearch(std::u32string_view pattern,
                                                 vtbackend::SearchMatchTally tally)
{
    if (pattern.empty())
        return {};

    if (tally.empty())
        return SearchStatus { .summary = "No results",
                              .outcome = SearchOutcome::NoMatch,
                              .navigation = MatchNavigation::Unavailable };

    // A capped tally is a floor, not a total, and the trailing "+" is the only thing that says so.
    auto const* const more = tally.exactness == vtbackend::TallyExactness::Capped ? "+" : "";

    // "3 of 27" is a claim about standing ON a match. When the caller is not on one -- they scrolled,
    // or the pattern came from a double-clicked word rather than from stepping -- there is no third
    // number to report, so the count is reported on its own instead of inventing a position.
    auto summary = tally.ordinal != 0
                       ? std::format("{} of {}{}", tally.ordinal, tally.total, more)
                       : std::format("{}{} {}", tally.total, more, tally.total == 1 ? "match" : "matches");

    // Somewhere else to go = more than one match, or exactly one that we are not already on.
    auto const navigation =
        (tally.total > 1 || tally.ordinal != 1) ? MatchNavigation::Available : MatchNavigation::Unavailable;

    return SearchStatus { .summary = std::move(summary),
                          .outcome = SearchOutcome::Matched,
                          .navigation = navigation };
}

} // namespace contour::session
