// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/links/ScanText.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

class Grid;
class SearchMatchMemo;

namespace detail
{
    /// One cell of a segment's subject: the units it spells and the columns it covers.
    struct SearchSubjectCell
    {
        std::size_t firstUnit = 0; ///< Its first unit in SearchSubject::units.
        std::size_t endUnit = 0;   ///< One past its last unit.
        CellLocation first {};     ///< Its column.
        CellLocation last {};      ///< Its last column: a wide character's second half.

        bool operator==(SearchSubjectCell const&) const = default;
    };

    struct SearchSubject;
} // namespace detail

/// How many cells a regex is fed as one text: a regex segment's budget, and the measure of what one bounded
/// read -- a walk's step, the keystroke's probe -- may cost the engine (@see SearchReadBudget).
///
/// Derived from cost. libc++'s std::wregex is quadratic in its subject for patterns such as `.*timeout`,
/// `\w+q` or `a*q` that fail at every start without ever tripping its complexity limit: measured on Apple
/// clang -O2, 1 024 units take 23 ms, 2 048 take 95 ms and 8 192 take 1.5 s (Debug: about nine times
/// that), all under the terminal lock. At 2 048 one segment stays under a tenth of a second at its very
/// worst, and an ordinary pattern over ordinary text far under a frame; a long logical line's regex
/// matches across its cuts are not found (R-d).
inline constexpr std::size_t RegexSegmentCells = 2048;

/// The rows of one logical line a search matches as one text: at most a query's segment budget of cells
/// (SearchQuery::segmentCells: @ref MaxScanCells for a literal, @ref RegexSegmentCells for a regex).
///
/// A long logical line is cut from its head every `max(1, cells / columns)` rows, so every reader of the
/// grid -- stepping, counting, highlighting -- cuts it at the same rows under the same query and sees the
/// same matches. A regex match across a cut is not found, as the link scanner does not find one; a literal
/// one is, since a literal spans no more cells than it has codepoints.
struct SearchSegment
{
    LineOffset first {}; ///< Its top row.
    LineOffset last {};  ///< Its bottom row: the same logical line as @ref first.

    bool operator==(SearchSegment const&) const = default;
};

/// The segment holding @p row.
/// @param grid  The grid.
/// @param row   A row of the grid, history or page.
/// @param cells The segment budget, in cells (SearchQuery::segmentCells).
/// @return Its segment: cut from the head of @p row's logical line, and ending at that line's end, the cut
///         or the page's bottom, whichever comes first.
[[nodiscard]] SearchSegment searchSegmentAt(Grid const& grid, LineOffset row, std::size_t cells);

/// The segment right below @p segment.
/// @param grid    The grid.
/// @param segment A segment of @p grid, cut at @p cells.
/// @param cells   The segment budget, in cells (SearchQuery::segmentCells).
/// @return It, or nothing where @p segment ends at the page's bottom.
[[nodiscard]] std::optional<SearchSegment> nextSearchSegment(Grid const& grid,
                                                             SearchSegment segment,
                                                             std::size_t cells);

/// The segment right above @p segment.
/// @param grid    The grid.
/// @param segment A segment of @p grid, cut at @p cells.
/// @param cells   The segment budget, in cells (SearchQuery::segmentCells).
/// @return It, or nothing where @p segment begins at the history's top.
[[nodiscard]] std::optional<SearchSegment> previousSearchSegment(Grid const& grid,
                                                                 SearchSegment segment,
                                                                 std::size_t cells);

class SearchQuery;

/// What a bounded read -- a walk's step, the keystroke's probe -- may still take, in whole segments.
///
/// Rows bound every read. A regex's is also bounded by what it costs the engine, which grows with the square
/// of a segment (@ref RegexSegmentCells): the read stops before a segment that would take the sum of its
/// segments' squared lengths past RegexSegmentCells squared, and starts no second segment longer than half
/// RegexSegmentCells. A page of short lines is so read whole, and a long logical line one segment at a time.
/// The first segment is always read.
class SearchReadBudget
{
  public:
    /// A bounded read.
    /// @param query   The query read for.
    /// @param rows    The rows it covers at most.
    /// @param columns The grid's width: a segment's length is its rows times this.
    SearchReadBudget(SearchQuery const& query, LineCount rows, ColumnCount columns) noexcept;

    /// A read bounded by rows alone, whatever the query: one that must reach as far as it is asked to.
    /// @param rows The rows it covers at most.
    /// @return The budget.
    [[nodiscard]] static SearchReadBudget rowsOnly(LineCount rows) noexcept;

    /// Takes @p segment on, where the budget allows it.
    /// @param segment The segment the read would go on to.
    /// @param rows    The rows of it the caller counts as read.
    /// @return Whether it may be read; once not, exhausted() says so.
    [[nodiscard]] bool admit(SearchSegment segment, LineCount rows) noexcept;

    /// @return Whether a segment was refused: the read stopped short of where it was going.
    [[nodiscard]] bool exhausted() const noexcept { return _exhausted; }

  private:
    SearchReadBudget(LineCount rows, ColumnCount columns, bool costBounded) noexcept;

    LineCount _rows;
    std::size_t _columns;
    bool _costBounded;
    LineCount _rowsRead {};
    std::size_t _segments = 0;
    std::size_t _cost = 0; ///< The sum of the squared lengths of the segments read, in cells.
    bool _exhausted = false;
};

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

    /// @return The cells one of its segments holds at most: @ref RegexSegmentCells for a regex, whose cost
    ///         grows faster than its subject; @ref MaxScanCells for a literal, whose cost does not.
    [[nodiscard]] std::size_t segmentCells() const noexcept
    {
        return _regex ? RegexSegmentCells : MaxScanCells;
    }

    /// @return Whether a read must also be bounded by the engine's cost: a regex's grows faster than its
    ///         subject, a literal's does not.
    [[nodiscard]] bool costGrowsFasterThanItsSubject() const noexcept { return _regex.has_value(); }

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
    friend class SearchMatchMemo;

    /// @p segment of @p grid as the matcher reads it. @see matchesIn
    [[nodiscard]] detail::SearchSubject subjectOf(Grid const& grid, SearchSegment segment) const;

    /// The matches in @p subject. @see matchesIn
    [[nodiscard]] std::expected<std::vector<CellLocationRange>, SearchPatternError> matchesOf(
        detail::SearchSubject const& subject) const;

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

/// A regex's matches per segment, kept while the segment reads as it did when they were found, so a visible
/// segment that a slow pattern takes long over is matched once rather than on every frame.
///
/// Reading a segment to compare costs as much as reading it to match a literal does; only the regex engine's
/// own work, which grows faster than its subject, is saved. Not thread-safe: the terminal's lock guards it,
/// as it guards the grid it reads.
class SearchMatchMemo
{
  public:
    /// @p query's matches in @p segment of @p grid: the remembered ones where the segment's text and layout
    /// are as they were under the same query, the same grid generation and the same top row; otherwise
    /// found now and remembered. A literal query is matched every time.
    [[nodiscard]] std::expected<std::vector<CellLocationRange>, SearchPatternError> matchesIn(
        std::shared_ptr<SearchQuery const> const& query, Grid const& grid, SearchSegment segment);

    /// Forgets everything: there is no query whose matches it could hold.
    void clear() noexcept
    {
        _query.reset();
        _entries.clear();
    }

    /// @return How often it has run the regex engine rather than remembered: a cost, read by the tests.
    [[nodiscard]] std::size_t engineRuns() const noexcept { return _engineRuns; }

  private:
    /// One segment's matches, its lines relative to its top row.
    struct Entry
    {
        int64_t firstRow = 0;                         ///< Its top row's stable id.
        std::wstring units;                           ///< What the engine read.
        std::vector<detail::SearchSubjectCell> cells; ///< Which cells spelled which units.
        std::vector<CellLocationRange> matches;       ///< What it found.
    };

    /// More than a page shows: a page of one-row lines is a few hundred segments at most.
    static constexpr std::size_t MaxEntries = 512;

    std::shared_ptr<SearchQuery const> _query;
    uint64_t _generation = 0;
    std::vector<Entry> _entries;
    std::size_t _engineRuns = 0;
};

/// @p text with every regex metacharacter escaped, so a regex of it matches the text literally.
/// @param text Any text.
/// @return @p text with a backslash before each of `\^$.|?*+()[]{}`.
[[nodiscard]] std::u32string escapeSearchRegex(std::u32string_view text);

} // namespace vtbackend
