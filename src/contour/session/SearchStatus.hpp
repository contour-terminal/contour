// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Search.hpp>

#include <crispy/Assert.hpp>

#include <cstdint>
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

/// Whether a search that has not been tallied yet matched anything at all.
///
/// Known the moment the search runs, unlike the count, which costs a walk of the whole grid. It is
/// what lets the bar tint the field and enable its buttons immediately while the count catches up.
enum class MatchPresence : uint8_t
{
    None = 0,
    Some,
};

/// Whether the tally in a @c SearchStatus is authoritative yet.
enum class CountReadiness : uint8_t
{
    Settled = 0, //!< The tally describes the grid as it stands.
    Counting,    //!< A tally is pending; only the outcome is known.
};

/// Everything the find bar derives from the terminal's search state.
///
/// Deliberately no rendered text: the words are the Qt layer's business, because they have to go
/// through tr() to be translatable, and this header is Qt-free on purpose. What is decided here is
/// WHICH thing to say and whether stepping is possible -- the parts with rules worth testing.
struct SearchStatus
{
    SearchOutcome outcome = SearchOutcome::Idle;
    MatchNavigation navigation = MatchNavigation::Unavailable;
    CountReadiness readiness = CountReadiness::Settled;
    vtbackend::SearchMatchTally tally {};
};

/// Derives the bar's state from the active pattern and its completed tally.
///
/// @param pattern What is being searched for. Empty means no search is active.
/// @param tally   The match tally for @p pattern. @see vtbackend::Terminal::tallySearchMatches.
[[nodiscard]] inline SearchStatus describeSearch(std::u32string_view pattern,
                                                 vtbackend::SearchMatchTally tally)
{
    if (pattern.empty())
        return {};

    if (tally.empty())
        return SearchStatus { .outcome = SearchOutcome::NoMatch,
                              .navigation = MatchNavigation::Unavailable,
                              .readiness = CountReadiness::Settled,
                              .tally = tally };

    // Somewhere else to go = more than one match, or exactly one that we are not already on. The bar
    // WRAPS (TerminalSession::wrapSearchTo), so being on the last of several is still navigable --
    // Terminal::searchNextMatch alone does not wrap, which is why the wrap lives in the session.
    auto const navigation =
        (tally.total > 1 || tally.ordinal != 1) ? MatchNavigation::Available : MatchNavigation::Unavailable;

    return SearchStatus { .outcome = SearchOutcome::Matched,
                          .navigation = navigation,
                          .readiness = CountReadiness::Settled,
                          .tally = tally };
}

/// The bar's state for a search that has run but not yet been counted.
///
/// Typing re-runs the search on every keystroke, and counting its matches walks the whole grid --
/// so the count is debounced while this fills the gap. Everything here is known without the walk:
/// whether anything matched (the field's tint) and whether stepping will go somewhere.
[[nodiscard]] inline SearchStatus describeSearchCounting(MatchPresence presence)
{
    if (presence == MatchPresence::None)
        return SearchStatus { .outcome = SearchOutcome::NoMatch,
                              .navigation = MatchNavigation::Unavailable,
                              .readiness = CountReadiness::Settled };

    return SearchStatus { .outcome = SearchOutcome::Matched,
                          .navigation = MatchNavigation::Available,
                          .readiness = CountReadiness::Counting };
}

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
    /// respects case, "aa" for one that ignores it -- so the button reads without its tooltip. Not
    /// prose, so unlike the tooltip it stays here rather than moving to the translatable layer.
    std::string_view glyph;
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
        case vtbackend::SearchCaseSensitivity::Smart: return { .glyph = "Aa", .pinned = CasePinned::No };
        case vtbackend::SearchCaseSensitivity::Sensitive: return { .glyph = "Aa", .pinned = CasePinned::Yes };
        case vtbackend::SearchCaseSensitivity::Insensitive:
            return { .glyph = "aa", .pinned = CasePinned::Yes };
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

} // namespace contour::session
