// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/Search.hpp>

#include <expected>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

class Grid;

/// The rows of one logical line a search matches as one text: at most @ref MaxScanCells cells of it.
///
/// A long logical line is cut from its head every `max(1, MaxScanCells / columns)` rows, so every reader of
/// the grid -- stepping, counting, highlighting -- cuts it at the same rows and sees the same matches. A
/// regex match across a cut is not found, as the link scanner does not find one; a literal one is, since a
/// literal spans no more cells than it has codepoints.
struct SearchSegment
{
    LineOffset first {}; ///< Its top row.
    LineOffset last {};  ///< Its bottom row: the same logical line as @ref first.

    bool operator==(SearchSegment const&) const = default;
};

/// The segment holding @p row.
/// @param grid The grid.
/// @param row  A row of the grid, history or page.
/// @return Its segment: cut from the head of @p row's logical line, and ending at that line's end, the cut
///         or the page's bottom, whichever comes first.
[[nodiscard]] SearchSegment searchSegmentAt(Grid const& grid, LineOffset row);

/// The segment right below @p segment.
/// @param grid    The grid.
/// @param segment A segment of @p grid.
/// @return It, or nothing where @p segment ends at the page's bottom.
[[nodiscard]] std::optional<SearchSegment> nextSearchSegment(Grid const& grid, SearchSegment segment);

/// The segment right above @p segment.
/// @param grid    The grid.
/// @param segment A segment of @p grid.
/// @return It, or nothing where @p segment begins at the history's top.
[[nodiscard]] std::optional<SearchSegment> previousSearchSegment(Grid const& grid, SearchSegment segment);

/// A search pattern compiled once, with the options it was compiled under.
///
/// Immutable once compiled: everything a match needs -- the resolved case comparison, the folded needle or
/// the compiled regex, the word delimiters -- is fixed by compile(), so stepping, counting and highlighting
/// all read one answer to "what does this pattern match" and none of them re-derives it.
class SearchQuery
{
  public:
    /// Compiles @p pattern under @p options.
    ///
    /// A literal pattern always compiles. A regex is NFC-normalized, folded outside its escapes where the
    /// comparison folds (so `\D` stays `\D`), and compiled with @ref RegexEngineFlags. Smart case reads a
    /// regex's own characters only -- the unescaped ones and those of a class -- so `\d+` folds and `\dA`
    /// does not.
    ///
    /// @pre !pattern.empty()
    /// @param pattern        The pattern as the user gave it.
    /// @param options        How to read it.
    /// @param wordDelimiters The characters that end a word for SearchWordBoundary::WholeWord.
    /// @return The query, or why the pattern cannot be searched for.
    [[nodiscard]] static std::expected<SearchQuery, SearchPatternError> compile(
        std::u32string_view pattern, SearchOptions options, std::u32string_view wordDelimiters);

    /// @return The pattern as given to compile().
    [[nodiscard]] std::u32string const& pattern() const noexcept { return _pattern; }

    /// @return The options it was compiled under.
    [[nodiscard]] SearchOptions options() const noexcept { return _options; }

    /// @return How letter case is compared: the options' case policy resolved against the pattern.
    [[nodiscard]] CaseComparison comparison() const noexcept { return _comparison; }

    /// Every match whose first cell is in @p segment, in order.
    ///
    /// The segment is read as its cells' text: a cell's grapheme cluster NFC-normalized (and folded where
    /// the comparison folds), an empty cell a space, a cell with no text of its own nothing, a row with no
    /// text one line break no pattern matches across, the trailing empty cells nothing. A match begins on a
    /// cell's first character and ends on a cell's last -- never inside a cluster -- and is not empty.
    /// Literal matches overlap; a regex's do not. A literal match may end past the segment's last row, on
    /// the rows its logical line continues onto; a regex match ends within the segment.
    ///
    /// @param grid    The grid.
    /// @param segment One of its segments. @see searchSegmentAt
    /// @return The matches, each as its first cell and the last column it covers (a wide character's second
    ///         half where it ends on one); SearchPatternError::TooComplex where the regex engine gave up.
    [[nodiscard]] std::expected<std::vector<CellLocationRange>, SearchPatternError> matchesIn(
        Grid const& grid, SearchSegment segment) const;

  private:
    SearchQuery(std::u32string pattern,
                SearchOptions options,
                CaseComparison comparison,
                std::u32string wordDelimiters,
                std::wstring needle,
                std::optional<std::wregex> regex);

    std::u32string _pattern;           ///< As given.
    SearchOptions _options;            ///< As given.
    CaseComparison _comparison;        ///< The case policy resolved against the pattern.
    std::u32string _wordDelimiters;    ///< What ends a word, read at compile time.
    std::wstring _needle;              ///< Literal: the NFC, folded pattern, one unit per codepoint.
    std::optional<std::wregex> _regex; ///< Regex: the compiled pattern.
};

/// @p text with every regex metacharacter escaped, so a regex of it matches the text literally.
/// @param text Any text.
/// @return @p text with a backslash before each of `\^$.|?*+()[]{}`.
[[nodiscard]] std::u32string escapeSearchRegex(std::u32string_view text);

} // namespace vtbackend
