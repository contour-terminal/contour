// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vtbackend
{

// What a search IS -- its case policy, where its pattern came from, and how its matches are counted
// and painted. Deliberately free of core/Primitives.hpp, so that grid/, screen/, input/vi/ and the
// GUI can all name this vocabulary without dragging the whole primitive set behind it.

/// Whether a search compares letters by their case.
///
/// @c Smart is what the terminal has always done and is therefore zero: a needle carrying no
/// uppercase letter matches case-insensitively, and one that does carry an uppercase letter matches
/// exactly. The other two pin the answer, which is what issue #1410 asks for -- the find bar's "Aa"
/// affordance cycles the three, and @c search_case_sensitivity picks which one it opens with.
enum class SearchCaseSensitivity : uint8_t
{
    Smart = 0,   //!< Case-sensitive exactly when the needle contains an uppercase letter.
    Insensitive, //!< Never case-sensitive.
    Sensitive,   //!< Always case-sensitive.
};

/// Where the currently active search pattern came from.
///
/// It selects between two named highlight vocabularies rather than reporting a yes/no fact: a
/// pattern the user typed paints @c ColorPalette::searchHighlight, while one the terminal derived
/// from a double-click paints @c wordHighlight. @see RenderBufferBuilder. It also decides how the
/// pattern is read: a word taken from the screen is text, never a regex, whatever the find bar's
/// toggles say.
enum class SearchOrigin : uint8_t
{
    Typed = 0,       //!< Entered into the find bar.
    DoubleClick,     //!< Derived from the selection, to visualize the selected word.
    WordUnderCursor, //!< The word under the cursor, as vi's `*` and `#` search for it.
};

/// Whether the renderer paints search matches at all.
///
/// Non-empty search patterns force the per-cell render path (@see Terminal::refreshRenderBuffer), so
/// this is what keeps a terminal with no active search on the cheaper one.
enum class HighlightSearchMatches : uint8_t
{
    No = 0,
    Yes
};

/// Whether a @c SearchMatchTally saw the whole grid.
///
/// A tally walks the scrollback, which is unbounded, so it stops at a limit and says so rather than
/// making the caller guess whether a round number is real. @see DefaultSearchMatchLimit.
enum class TallyExactness : uint8_t
{
    Exact = 0, //!< Every match in the grid was counted.
    Capped,    //!< Counting stopped at the limit; there are at least @c total matches.
};

/// How many matches a tally counts before giving up, by default.
///
/// Chosen so the capped count still renders in the width the exact one needs: "9999+" is five
/// characters, as "10000" would be. The find bar shows it verbatim.
constexpr inline size_t DefaultSearchMatchLimit = 9999;

/// A "3 of 27" summary of where one position sits among the current pattern's matches.
///
/// Both numbers come from one walk of the grid: counting the matches and finding which one the
/// caller is standing on are the same traversal, and doing them separately would walk the scrollback
/// twice to answer one label.
struct SearchMatchTally
{
    size_t total = 0;   //!< Matches found, at most the limit the caller passed.
    size_t ordinal = 0; //!< 1-based index of the match at the queried position; 0 when it is not on one.
    TallyExactness exactness = TallyExactness::Exact;

    /// Whether there is anything to step between.
    [[nodiscard]] constexpr bool empty() const noexcept { return total == 0; }

    constexpr bool operator==(SearchMatchTally const&) const noexcept = default;
};

/// A case policy resolved against an actual needle: how the comparison will be done.
///
/// Distinct from @c SearchCaseSensitivity, which is what the user asked for -- @c Smart is a rule,
/// not an answer. This is the answer, which SearchQuery::compile() settles once per pattern and folds the
/// needle and the subject by; named rather than left a @c bool, by this project's own rule.
enum class CaseComparison : uint8_t
{
    Folded = 0, //!< Letters compare equal regardless of case.
    Exact,      //!< Letters must match case.
};

/// Resolves @p mode against @p searchText.
///
/// The single answer to that question in the tree. It used to be asked twice and answered
/// differently: @c Screen matched via the UCD, while @c RenderBufferBuilder highlighted via
/// @c std::isupper -- which is undefined for every codepoint above 0xFF and answers for the wrong
/// alphabet besides, so "Привет" matched case-sensitively and highlighted case-insensitively.
///
/// @param searchText The needle to inspect. Only read under @c SearchCaseSensitivity::Smart.
/// @param mode       The configured policy.
/// @return How the comparison must treat letter case.
[[nodiscard]] CaseComparison caseComparisonFor(std::u32string_view searchText,
                                               SearchCaseSensitivity mode) noexcept;

/// How a search pattern is read.
enum class SearchSyntax : uint8_t
{
    Literal = 0, //!< The pattern is the text to find, character for character.
    Regex,       //!< The pattern is an ECMAScript regular expression.
};

/// Where a match may begin and end relative to the words around it.
///
/// A search segment's cut counts as a word boundary -- a regex's cuts on both sides; a literal's only where a
/// match begins, since a literal reads on past its segment's end. A long logical line is cut into segments (a
/// regex's every RegexSegmentCells cells), and a word a cut runs through reads there as a line's end would.
/// The cuts fall at fixed rows, so this is rare, and the same for every reader.
enum class SearchWordBoundary : uint8_t
{
    Any = 0,   //!< Anywhere, inside a word too.
    WholeWord, //!< Only where a word delimiter, a line's end or a line break is on either side.
};

/// How a search reads its pattern: the find bar's three toggles.
///
/// All-zero is what the terminal has always done -- smart case, literal text, anywhere -- so a
/// value-initialized one changes nothing.
struct SearchOptions
{
    SearchCaseSensitivity caseSensitivity = SearchCaseSensitivity::Smart; //!< Letter case.
    SearchSyntax syntax = SearchSyntax::Literal;                          //!< Literal or regex.
    SearchWordBoundary wordBoundary = SearchWordBoundary::Any;            //!< Whole words only, or not.

    constexpr bool operator==(SearchOptions const&) const noexcept = default;
};

/// Why a search pattern cannot be searched for.
enum class SearchPatternError : uint8_t
{
    InvalidSyntax = 0, //!< The regex does not compile.
    BackReference,     //!< The regex names a back-reference (`\1`…), which no engine here runs everywhere.
    TooComplex,        //!< Matching gave up within the engine's own complexity or stack budget.
};

/// Whether a search setter changed what is searched for, so the caller knows whether to search again.
enum class SearchChange : uint8_t
{
    Unchanged = 0, //!< Everything was already so; the last search's answer stands.
    Changed,       //!< The pattern or how it is read changed.
};

} // namespace vtbackend
