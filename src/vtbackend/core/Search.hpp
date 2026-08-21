// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace vtbackend
{

// What a search IS -- its case policy, where its pattern came from, and how its matches are counted
// and painted. Deliberately free of core/Primitives.hpp, so that grid/, screen/, input/vi/ and the
// GUI can all name this vocabulary without dragging the whole primitive set behind it. The one
// search type that stays behind is SearchResult, whose ColumnOffset member would require exactly
// that include.

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
/// from a double-click paints @c wordHighlight. @see RenderBufferBuilder.
enum class SearchOrigin : uint8_t
{
    Typed = 0,   //!< Entered into the find bar, or by a vi-mode search.
    DoubleClick, //!< Derived from the selection, to visualize the selected word.
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
    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return total == 0;
    } // NOLINT(readability-identifier-naming)
};

constexpr bool operator==(SearchMatchTally a, SearchMatchTally b) noexcept
{
    return a.total == b.total && a.ordinal == b.ordinal && a.exactness == b.exactness;
}

/// Decides whether a needle is matched case-sensitively under @p mode.
///
/// The single answer to that question in the tree. It used to be asked twice and answered
/// differently: @c Screen matched via the UCD, while @c RenderBufferBuilder highlighted via
/// @c std::isupper -- which is undefined for every codepoint above 0xFF and answers for the wrong
/// alphabet besides, so "Привет" matched case-sensitively and highlighted case-insensitively.
///
/// @param searchText The needle to inspect. Only read under @c SearchCaseSensitivity::Smart.
/// @param mode       The configured policy.
/// @return true if the comparison must respect letter case.
[[nodiscard]] bool isCaseSensitiveSearch(std::u32string_view searchText, SearchCaseSensitivity mode) noexcept;

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::SearchCaseSensitivity>: formatter<std::string_view>
{
    auto format(vtbackend::SearchCaseSensitivity value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::SearchCaseSensitivity::Smart: name = "Smart"; break;
            case vtbackend::SearchCaseSensitivity::Insensitive: name = "Insensitive"; break;
            case vtbackend::SearchCaseSensitivity::Sensitive: name = "Sensitive"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::SearchOrigin>: formatter<std::string_view>
{
    auto format(vtbackend::SearchOrigin value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::SearchOrigin::Typed: name = "Typed"; break;
            case vtbackend::SearchOrigin::DoubleClick: name = "DoubleClick"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::TallyExactness>: formatter<std::string_view>
{
    auto format(vtbackend::TallyExactness value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::TallyExactness::Exact: name = "Exact"; break;
            case vtbackend::TallyExactness::Capped: name = "Capped"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::SearchMatchTally>: formatter<std::string>
{
    auto format(vtbackend::SearchMatchTally value, auto& ctx) const
    {
        return formatter<std::string>::format(
            std::format("{} of {}{}",
                        value.ordinal,
                        value.total,
                        value.exactness == vtbackend::TallyExactness::Capped ? "+" : ""),
            ctx);
    }
};
