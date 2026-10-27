// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/SearchQuery.hpp>

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/RegexEngine.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/grid/LineSoA.hpp>
#include <vtbackend/links/ScanText.hpp>

#include <libunicode/case_mapping.h>
#include <libunicode/normalization.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

namespace
{
    /// The characters a regex gives a meaning of their own, which escapeSearchRegex() escapes.
    constexpr auto RegexMetacharacters = std::u32string_view { U"\\^$.|?*+()[]{}" };

    /// Appends @p codepoint to @p units as the regex engine's character type spells it: one unit where
    /// @c wchar_t is UTF-32 (macOS, Linux), a surrogate pair above U+FFFF where it is UTF-16 (Windows).
    void appendUnits(std::wstring& units, char32_t codepoint)
    {
        if constexpr (sizeof(wchar_t) >= sizeof(char32_t))
            units.push_back(static_cast<wchar_t>(codepoint));
        else if (codepoint < 0x10000)
            units.push_back(static_cast<wchar_t>(codepoint));
        else
        {
            auto const offset = codepoint - 0x10000;
            units.push_back(static_cast<wchar_t>(0xD800 + (offset >> 10)));
            units.push_back(static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)));
        }
    }

    /// @p codepoint as a comparison under @p comparison sees it.
    [[nodiscard]] char32_t foldedFor(char32_t codepoint, CaseComparison comparison) noexcept
    {
        switch (comparison)
        {
            case CaseComparison::Folded: return unicode::simple_lowercase(codepoint);
            case CaseComparison::Exact: break;
        }
        return codepoint;
    }

    /// Whether a character in @p role is one of the regex's own, rather than one an escape names.
    [[nodiscard]] constexpr bool isUnescaped(RegexSourceRole role) noexcept
    {
        switch (role)
        {
            case RegexSourceRole::Plain:
            case RegexSourceRole::InClass: return true;
            case RegexSourceRole::Escaped:
            case RegexSourceRole::EscapedInClass: return false;
        }
        return false;
    }

    /// Tells a class's negating `^` -- the first character after the `[` that opens it -- from its items, fed
    /// the characters of a source in order with the roles they have there.
    class ClassNegationMarker
    {
      public:
        /// @return Whether @p ch, in @p role, is a class's negating `^`.
        [[nodiscard]] bool negates(char32_t ch, RegexSourceRole role) noexcept
        {
            auto const isMarker = _classJustOpened && role == RegexSourceRole::InClass && ch == U'^';
            _classJustOpened = role == RegexSourceRole::Plain && ch == U'[';
            return isMarker;
        }

      private:
        bool _classJustOpened = false; ///< Whether the last character opened a class.
    };

    /// The characters of @p source that smart case reads: those the regex spells itself, not its escapes.
    [[nodiscard]] std::u32string unescapedCharactersOf(std::u32string_view source)
    {
        auto characters = std::u32string {};
        (void) anyRegexSourceCharacter(source, [&](char32_t ch, RegexSourceRole role) {
            if (isUnescaped(role))
                characters.push_back(ch);
            return false;
        });
        return characters;
    }

    /// Whether a class in @p source holds a range whose ends are reversed, as `[z-a]`.
    ///
    /// The standard calls that an error, and libstdc++ and newer libc++ throw for it; Apple's libc++ compiles
    /// it into a class that matches nothing. Refused here, so a pattern is invalid on every platform alike.
    /// An end spelled by an escape is not judged.
    [[nodiscard]] bool hasReversedClassRange(std::u32string_view source)
    {
        auto low = std::optional<char32_t> {};
        auto inRange = false;
        auto marker = ClassNegationMarker {};
        return anyRegexSourceCharacter(source, [&](char32_t ch, RegexSourceRole role) {
            auto const endsRun =
                marker.negates(ch, role) || role != RegexSourceRole::InClass || ch == U']' || ch == U'\\';
            if (endsRun)
            {
                low.reset();
                inRange = false;
                return false;
            }
            if (inRange)
            {
                auto const reversed = *low > ch;
                low.reset();
                inRange = false;
                return reversed;
            }
            if (ch == U'-' && low)
            {
                inRange = true;
                return false;
            }
            low = ch;
            return false;
        });
    }

    /// A literal needle as the subject spells it: NFC, folded under @p comparison, in engine units.
    [[nodiscard]] std::wstring literalUnits(std::u32string_view pattern, CaseComparison comparison)
    {
        auto units = std::wstring {};
        for (auto const codepoint: unicode::to_nfc(pattern))
            appendUnits(units, foldedFor(codepoint, comparison));
        return units;
    }

    /// Appends to @p units the lowercase images of the uppercase codepoints in [@p low, @p high], as class
    /// items: what a range of a folded class must also match, because the subject it runs on is folded.
    void appendFoldedImages(std::wstring& units, char32_t low, char32_t high)
    {
        auto images = std::vector<char32_t> {};
        for (auto const codepoint:
             std::views::iota(static_cast<uint32_t>(low), static_cast<uint32_t>(high) + 1))
            if (auto const lower = unicode::simple_lowercase(static_cast<char32_t>(codepoint));
                lower != codepoint)
                images.push_back(lower);
        std::ranges::sort(images);
        auto const [unique, end] = std::ranges::unique(images);
        images.erase(unique, end);
        // As runs, so [A-Z] adds one a-z rather than 26 items.
        auto runStart = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, images.size()))
        {
            if (index + 1 < images.size() && images[index + 1] == images[index] + 1)
                continue;
            appendUnits(units, images[runStart]);
            if (index > runStart)
            {
                units.push_back(L'-');
                appendUnits(units, images[index]);
            }
            runStart = index + 1;
        }
    }

    /// A regex's source as the engine compiles it: NFC, folded outside its escapes under @p comparison.
    ///
    /// A class range keeps its ends as written -- folding them would reverse `[Z-a]` and widen `[@-Z]` over
    /// `_` -- and gains the lowercase images of the capitals it spans, so `[A-Z]` still matches the folded
    /// subject's letters.
    [[nodiscard]] std::wstring regexUnits(std::u32string_view pattern, CaseComparison comparison)
    {
        auto units = std::wstring {};
        auto const normalized = unicode::to_nfc(pattern);
        // A class item not yet written: it may be a range's low end.
        auto low = std::optional<char32_t> {};
        auto inRange = false;
        auto const flush = [&] {
            if (low)
                appendUnits(units, foldedFor(*low, comparison));
            if (inRange)
                units.push_back(L'-');
            low.reset();
            inRange = false;
        };
        auto marker = ClassNegationMarker {};
        (void) anyRegexSourceCharacter(
            std::u32string_view(normalized), [&](char32_t ch, RegexSourceRole role) {
                auto const isClassItem = !marker.negates(ch, role) && role == RegexSourceRole::InClass
                                         && ch != U']' && ch != U'\\';
                if (!isClassItem)
                {
                    flush();
                    appendUnits(units, isUnescaped(role) ? foldedFor(ch, comparison) : ch);
                }
                else if (inRange)
                {
                    appendUnits(units, *low);
                    units.push_back(L'-');
                    appendUnits(units, ch);
                    if (comparison == CaseComparison::Folded && *low <= ch)
                        appendFoldedImages(units, *low, ch);
                    low.reset();
                    inRange = false;
                }
                else if (ch == U'-' && low)
                    inRange = true;
                else
                {
                    flush();
                    low = ch;
                }
                return false;
            });
        flush();
        return units;
    }

    /// The rows a segment holds at most, from its logical line's head: @ref MaxScanCells cells' worth.
    [[nodiscard]] int rowsPerSegment(Grid const& grid) noexcept
    {
        return std::max(1, static_cast<int>(MaxScanCells) / std::max(1, unbox<int>(grid.pageSize().columns)));
    }

    /// One cell of a segment's subject: the units it spells and the columns it covers.
    struct SubjectCell
    {
        std::size_t firstUnit = 0; ///< Its first unit in Subject::units.
        std::size_t endUnit = 0;   ///< One past its last unit.
        CellLocation first {};     ///< Its column.
        CellLocation last {};      ///< Its last column: a wide character's second half.
    };

    /// A segment as the matcher reads it (R-a): its cells' text in order, and which cells spelled it.
    struct Subject
    {
        std::wstring units;             ///< The text, in the regex engine's units.
        std::vector<SubjectCell> cells; ///< Every cell that spelled units, in order.
        std::size_t segmentCells = 0; ///< How many of @ref cells are the segment's; a match begins in those.
    };

    /// Whether @p line shows no text at all, and so reads as one line break: unmaterialized, or every cell
    /// of it empty. A row of nothing but cells with no text of their own -- a tall block's lower part -- is
    /// not one: those cells belong to the text above, and read as nothing.
    [[nodiscard]] bool readsAsLineBreak(Line const& line)
    {
        if (line.isBlank())
            return true;
        auto const& storage = line.storage();
        return std::ranges::all_of(std::views::iota(std::size_t { 0 }, unbox<std::size_t>(line.size())),
                                   [&storage](std::size_t column) {
                                       return storage.clusterSize[column] == 0
                                              && !holdsNoText(storage, column);
                                   });
    }

    /// Appends the cluster of cell @p column of @p storage to @p units: NFC, folded under @p comparison.
    void appendCluster(std::wstring& units,
                       LineSoA const& storage,
                       std::size_t column,
                       CaseComparison comparison)
    {
        // The common case, one codepoint below the combining marks, is already NFC.
        if (auto const lead = storage.codepoints[column]; storage.clusterSize[column] == 1 && lead < 0x300)
        {
            appendUnits(units, foldedFor(lead, comparison));
            return;
        }
        auto cluster = std::u32string {};
        forEachCodepoint(storage, column, [&cluster](char32_t codepoint) { cluster.push_back(codepoint); });
        for (auto const codepoint: unicode::to_nfc(cluster))
            appendUnits(units, foldedFor(codepoint, comparison));
    }

    /// Reads @p segment of @p grid as the matcher's subject, and @p lookahead cells of its logical line past
    /// its end: a literal match that begins in the segment may end beyond a cut (R-d).
    [[nodiscard]] Subject subjectOf(Grid const& grid,
                                    SearchSegment segment,
                                    CaseComparison comparison,
                                    std::size_t lookahead)
    {
        auto subject = Subject {};
        // Where the text ends once the trailing empty cells and line breaks are dropped.
        auto textUnits = std::size_t { 0 };
        auto textCells = std::size_t { 0 };
        auto const appendRow = [&](LineOffset row) {
            auto const& line = grid.lineAt(row);
            if (readsAsLineBreak(line))
            {
                subject.units.push_back(L'\n');
                return;
            }
            auto const& storage = line.storage();
            auto const columns = unbox<int>(line.size());
            for (auto const column: std::views::iota(0, columns))
            {
                auto const index = static_cast<std::size_t>(column);
                if (holdsNoText(storage, index))
                    continue;
                auto const firstUnit = subject.units.size();
                auto const isEmpty = storage.clusterSize[index] == 0;
                if (isEmpty)
                    subject.units.push_back(L' ');
                else
                    appendCluster(subject.units, storage, index, comparison);
                auto const width = std::max(1, static_cast<int>(storage.widths[index]));
                subject.cells.push_back(SubjectCell {
                    .firstUnit = firstUnit,
                    .endUnit = subject.units.size(),
                    .first = CellLocation { .line = row, .column = ColumnOffset(column) },
                    .last = CellLocation { .line = row,
                                           .column = ColumnOffset(std::min(column + width, columns) - 1) },
                });
                if (!isEmpty)
                {
                    textUnits = subject.units.size();
                    textCells = subject.cells.size();
                }
            }
        };

        for (auto const row: std::views::iota(unbox<int>(segment.first), unbox<int>(segment.last) + 1))
            appendRow(LineOffset(row));
        subject.segmentCells = subject.cells.size();

        // Each cell spells at least one unit, so as many cells as the needle has units are all it can reach.
        auto const lastPageLine = boxed_cast<LineOffset>(grid.pageSize().lines) - 1;
        for (auto const row: std::views::iota(unbox<int>(segment.last) + 1, unbox<int>(lastPageLine) + 1))
        {
            if (!grid.isLineWrapped(LineOffset(row))
                || subject.cells.size() - subject.segmentCells >= lookahead)
                break;
            appendRow(LineOffset(row));
        }

        subject.units.resize(textUnits);
        subject.cells.resize(textCells);
        subject.segmentCells = std::min(subject.segmentCells, textCells);
        return subject;
    }

    /// Whether @p unit ends a word for SearchWordBoundary::WholeWord (R-c).
    [[nodiscard]] bool delimitsWord(wchar_t unit, std::u32string_view delimiters) noexcept
    {
        return unit == L' ' || unit == L'\n' || delimiters.contains(static_cast<char32_t>(unit));
    }

    /// The match of units [@p begin, @p end) of @p subject, where it begins and ends on cell boundaries, is
    /// not empty, and stands as a whole word where @p boundary asks for one.
    [[nodiscard]] std::optional<CellLocationRange> acceptedMatch(Subject const& subject,
                                                                 std::size_t begin,
                                                                 std::size_t end,
                                                                 SearchWordBoundary boundary,
                                                                 std::u32string_view delimiters)
    {
        if (begin >= end)
            return std::nullopt;
        // A row with no text is one line break that no pattern matches across (R-a), not even `\s` or `[^x]`.
        if (std::wstring_view(subject.units).substr(begin, end - begin).contains(L'\n'))
            return std::nullopt;
        auto const firstCell = std::ranges::lower_bound(subject.cells, begin, {}, &SubjectCell::firstUnit);
        if (firstCell == subject.cells.end() || firstCell->firstUnit != begin)
            return std::nullopt;
        auto const lastCell = std::ranges::lower_bound(subject.cells, end, {}, &SubjectCell::endUnit);
        if (lastCell == subject.cells.end() || lastCell->endUnit != end)
            return std::nullopt;
        switch (boundary)
        {
            case SearchWordBoundary::Any: break;
            case SearchWordBoundary::WholeWord:
                if (begin > 0 && !delimitsWord(subject.units[begin - 1], delimiters))
                    return std::nullopt;
                if (end < subject.units.size() && !delimitsWord(subject.units[end], delimiters))
                    return std::nullopt;
                break;
        }
        return CellLocationRange { .first = firstCell->first, .second = lastCell->last };
    }

    /// Where a regex search resumes after dropping the match it found at @p begin: one unit on, or, for a
    /// whole word, just past the next delimiter -- no whole word begins before it, so stepping a unit at a
    /// time would only find, and drop, the matches in between.
    /// @return The unit to resume at; past the subject's end where nothing is left to find.
    [[nodiscard]] std::size_t resumeAfterRejection(Subject const& subject,
                                                   std::size_t begin,
                                                   SearchWordBoundary boundary,
                                                   std::u32string_view delimiters)
    {
        switch (boundary)
        {
            case SearchWordBoundary::Any: break;
            case SearchWordBoundary::WholeWord: {
                auto const rest = std::wstring_view(subject.units).substr(begin);
                auto const delimiter = std::ranges::find_if(
                    rest, [delimiters](wchar_t unit) { return delimitsWord(unit, delimiters); });
                return begin + static_cast<std::size_t>(delimiter - rest.begin()) + 1;
            }
        }
        return begin + 1;
    }
} // namespace

SearchQuery::SearchQuery(std::u32string pattern,
                         SearchOptions options,
                         CaseComparison comparison,
                         std::u32string wordDelimiters,
                         std::wstring needle,
                         std::optional<std::wregex> regex):
    _pattern { std::move(pattern) },
    _options { options },
    _comparison { comparison },
    _wordDelimiters { std::move(wordDelimiters) },
    _needle { std::move(needle) },
    _regex { std::move(regex) }
{
}

std::expected<SearchQuery, SearchPatternError> SearchQuery::compile(std::u32string_view pattern,
                                                                    SearchOptions options,
                                                                    std::u32string_view wordDelimiters)
{
    switch (options.syntax)
    {
        case SearchSyntax::Literal: {
            auto const comparison = caseComparisonFor(pattern, options.caseSensitivity);
            return SearchQuery {
                std::u32string(pattern),           options,     comparison, std::u32string(wordDelimiters),
                literalUnits(pattern, comparison), std::nullopt
            };
        }
        case SearchSyntax::Regex: break;
    }

    if (hasRegexBackReference(pattern))
        return std::unexpected { SearchPatternError::BackReference };

    if (hasReversedClassRange(pattern))
        return std::unexpected { SearchPatternError::InvalidSyntax };

    auto const comparison = caseComparisonFor(unescapedCharactersOf(pattern), options.caseSensitivity);
    try
    {
        auto regex = std::wregex(regexUnits(pattern, comparison), RegexEngineFlags);
        return SearchQuery { std::u32string(pattern),        options,         comparison,
                             std::u32string(wordDelimiters), std::wstring {}, std::move(regex) };
    }
    catch (std::regex_error const&)
    {
        return std::unexpected { SearchPatternError::InvalidSyntax };
    }
}

std::expected<std::vector<CellLocationRange>, SearchPatternError> SearchQuery::matchesIn(
    Grid const& grid, SearchSegment segment) const
{
    // A literal reads on past the segment's end by as many cells as it can span, so a match across a cut is
    // found by the segment it begins in; a regex reads the segment alone (R-d).
    auto const subject = subjectOf(grid, segment, _comparison, _regex ? 0 : _needle.size());
    auto const boundary = _options.wordBoundary;
    auto matches = std::vector<CellLocationRange> {};

    if (!_regex)
    {
        // Every start, overlapping, as a literal search steps one cell at a time.
        for (auto const& cell: std::span(subject.cells).first(subject.segmentCells))
            if (subject.units.compare(cell.firstUnit, _needle.size(), _needle) == 0)
                if (auto const match = acceptedMatch(
                        subject, cell.firstUnit, cell.firstUnit + _needle.size(), boundary, _wordDelimiters))
                    matches.push_back(*match);
        return matches;
    }

    try
    {
        // The engine's own iteration, except that a match it finds off a cell boundary, empty, across a row
        // with no text, or not a whole word where one is asked for is dropped, and the search resumes at the
        // next place a match may begin.
        auto position = std::size_t { 0 };
        auto found = std::wsmatch {}; // Reused: each search would otherwise allocate its own sub-matches.
        while (position <= subject.units.size())
        {
            auto const from = subject.units.begin() + static_cast<std::ptrdiff_t>(position);
            auto const flags =
                position == 0 ? std::regex_constants::match_default : std::regex_constants::match_prev_avail;
            if (!std::regex_search(from, subject.units.end(), found, *_regex, flags))
                break;
            auto const begin = position + static_cast<std::size_t>(found.position(0));
            auto const end = begin + static_cast<std::size_t>(found.length(0));
            if (auto const match = acceptedMatch(subject, begin, end, boundary, _wordDelimiters))
            {
                matches.push_back(*match);
                position = end;
            }
            else
                position = resumeAfterRejection(subject, begin, boundary, _wordDelimiters);
        }
    }
    catch (std::regex_error const&)
    {
        return std::unexpected { SearchPatternError::TooComplex };
    }
    return matches;
}

SearchSegment searchSegmentAt(Grid const& grid, LineOffset row)
{
    auto const head = grid.logicalLineHead(row);
    auto const rows = rowsPerSegment(grid);
    auto const first = head + LineOffset::cast_from(((unbox<int>(row) - unbox<int>(head)) / rows) * rows);
    auto const lastPageLine = boxed_cast<LineOffset>(grid.pageSize().lines) - 1;
    auto const limit = std::min(first + LineOffset::cast_from(rows - 1), lastPageLine);
    return SearchSegment { .first = first, .last = grid.logicalLineEnd(first, limit) };
}

std::optional<SearchSegment> nextSearchSegment(Grid const& grid, SearchSegment segment)
{
    auto const lastPageLine = boxed_cast<LineOffset>(grid.pageSize().lines) - 1;
    if (segment.last >= lastPageLine)
        return std::nullopt;
    auto const first = segment.last + 1;
    if (!grid.continuesRowAbove(first))
        return searchSegmentAt(grid, first);
    // The rest of the same logical line, cut where its head's cuts fall: no walk back up to the head, which
    // would make walking a long line's segments quadratic.
    auto const limit = std::min(first + LineOffset::cast_from(rowsPerSegment(grid) - 1), lastPageLine);
    return SearchSegment { .first = first, .last = grid.logicalLineEnd(first, limit) };
}

std::optional<SearchSegment> previousSearchSegment(Grid const& grid, SearchSegment segment)
{
    auto const historyTop = -boxed_cast<LineOffset>(grid.historyLineCount());
    if (segment.first <= historyTop)
        return std::nullopt;
    if (!grid.continuesRowAbove(segment.first))
        return searchSegmentAt(grid, segment.first - 1);
    // The cut above, within the same logical line, as nextSearchSegment() steps down without a head walk.
    return SearchSegment {
        .first = std::max(segment.first - LineOffset::cast_from(rowsPerSegment(grid)), historyTop),
        .last = segment.first - 1,
    };
}

std::u32string escapeSearchRegex(std::u32string_view text)
{
    auto escaped = std::u32string {};
    escaped.reserve(text.size());
    for (auto const ch: text)
    {
        if (RegexMetacharacters.contains(ch))
            escaped.push_back(U'\\');
        escaped.push_back(ch);
    }
    return escaped;
}

} // namespace vtbackend
