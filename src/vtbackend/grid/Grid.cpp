// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Grid.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <crispy/Assert.hpp>
#include <crispy/LogStore.hpp>

#include <algorithm>
#include <format>
#include <iostream>
#include <ranges>
#include <utility>

using std::max;
using std::min;
using std::u32string;
using std::u32string_view;
using std::vector;

namespace vtbackend
{

auto inline const gridLog = logstore::Category(
    "vt.grid", "Grid related", logstore::Category::State::Disabled, logstore::Category::Visibility::Hidden);

namespace detail
{
    static Lines createLines(PageSize pageSize,
                             LineCount maxHistoryLineCount,
                             bool reflowOnResize,
                             GraphicsAttributes initialSGR)
    {
        auto const defaultLineFlags = reflowOnResize ? LineFlag::Wrappable : LineFlag::None;
        auto const totalLineCount = unbox<size_t>(pageSize.lines + maxHistoryLineCount);

        Lines lines;
        lines.reserve(totalLineCount);

        for ([[maybe_unused]] auto const _: std::views::iota(0u, totalLineCount))
            lines.emplace_back(pageSize.columns, defaultLineFlags, initialSGR);

        return lines;
    }

    /// Internal linkage, matching the `static` helpers around it: this is reflow's own vocabulary and
    /// nothing outside this translation unit names it.
    namespace
    {
        /// Everything a logical line carries that survives being re-chopped into physical rows.
        ///
        /// One parameter rather than four, and one local rather than four kept in step by hand: reflow
        /// assigns these together, captures them together and passes them together, so a fifth attribute is
        /// a member here instead of an edit at six sites. It also un-swaps the two ColumnOffsets, which as
        /// adjacent parameters of one type could be exchanged at any call site without a diagnostic.
        struct LogicalLineAttributes
        {
            LineFlags flags = LineFlag::None;

            /// An offset into the LOGICAL line, carried by its head alone: re-chopping the line into
            /// different physical pieces leaves it untouched.
            ColumnOffset commandEndOffset {};

            /// Likewise head-only. @see commandEndOffset.
            ColumnOffset promptEndOffset {};

            /// NOT head-only, unlike the two above: those name a border WITHIN the logical line, while this
            /// names who WROTE it, and a wrap does not change the author of a continuation.
            ContextId contextId {};
        };
    } // namespace

    /// Splits a logical line (stored as a LineSoA) into fixed-width Line objects.
    /// @param attributes What the logical line carries; only its head keeps the head-only members.
    /// @returns number of inserted lines.
    static LineCount addNewWrappedLines(Lines& targetLines,
                                        ColumnCount newColumnCount,
                                        LineSoA const& logicalLineBuffer,
                                        size_t usedColumns,
                                        LogicalLineAttributes const& attributes,
                                        bool initialNoWrap)
    {
        auto const& baseFlags = attributes.flags;
        auto const newCols = unbox<size_t>(newColumnCount);
        int i = 0;
        size_t offset = 0;

        // Only the very first chunk of a logical line is that line's head, and only when the caller says
        // the split starts one (a shrink hands us the OVERFLOW of a head it already emitted, so every chunk
        // it asks for is a continuation). The head keeps the logical line's semantic marks and its
        // command-end offset — an offset into the LOGICAL line, so re-chopping that line into different
        // physical pieces leaves it untouched. A continuation keeps only the flags that describe a physical
        // line, and is marked as wrapped.
        auto const emitChunk = [&](LineSoA&& chunk) {
            auto const isHead = i == 0 && initialNoWrap;
            targetLines.emplace_back(isHead ? baseFlags
                                            : (baseFlags.without(HeadOnlyLineFlags) | LineFlag::Wrapped),
                                     std::move(chunk),
                                     newColumnCount);
            if (isHead)
            {
                targetLines.back().setCommandEndOffset(attributes.commandEndOffset);
                targetLines.back().setPromptEndOffset(attributes.promptEndOffset);
            }
            // EVERY chunk, head or not. @see LogicalLineAttributes::contextId.
            targetLines.back().adoptContext(attributes.contextId);
            ++i;
        };

        while (offset + newCols <= usedColumns)
        {
            LineSoA chunk;
            initializeLineSoA(chunk, newColumnCount);
            copyColumns(logicalLineBuffer, offset, chunk, 0, newCols);
            emitChunk(std::move(chunk));
            offset += newCols;
        }

        if (offset < usedColumns)
        {
            auto const remaining = usedColumns - offset;
            LineSoA chunk;
            initializeLineSoA(chunk, newColumnCount);
            copyColumns(logicalLineBuffer, offset, chunk, 0, remaining);
            emitChunk(std::move(chunk));
        }
        return LineCount::cast_from(i);
    }

} // namespace detail
// {{{ Grid impl
Grid::Grid(PageSize pageSize, bool reflowOnResize, HistoryLimits historyLimits):
    _pageSize { pageSize },
    _reflowOnResize { reflowOnResize },
    _historyLimits { historyLimits },
    _lines { detail::createLines(
        pageSize,
        [capacity = historyLimits.capacity]() -> LineCount {
            if (auto const* maxLineCount = std::get_if<LineCount>(&capacity))
                return *maxLineCount;
            else
                return LineCount::cast_from(0);
        }(),
        reflowOnResize,
        GraphicsAttributes {}) },
    _linesUsed { pageSize.lines }
{
    verifyState();
}

void Grid::setHistoryLimits(HistoryLimits historyLimits)
{
    verifyState();
    rezeroBuffers();
    _historyLimits = historyLimits;

    // Slots appear and vanish at the boundary between the page and the scrollback, NOT at the
    // ring's end.
    //
    // After rezeroBuffers() the ring reads [page][free][history] with the history newest-last, so
    // the ring's end is where the NEWEST history row lives. Resizing there gets both directions
    // exactly backwards: growing splices blanks between that row and the page, blanking the top of
    // the scrollback and sliding the real rows out of the addressable range, and shrinking drops
    // the newest rows while keeping the oldest. The free region at `_pageSize.lines` is the one
    // place where slots may come and go without moving a row relative to offset 0, and erasing
    // past it consumes the OLDEST history rows first -- precisely the end a shrunken scrollback
    // has to forget.
    auto const newTotalLineCount = _pageSize.lines + this->maxHistoryLineCount();
    auto const oldSlotCount = _lines.size();
    auto const newSlotCount = unbox<size_t>(newTotalLineCount);
    auto& slots = _lines.storage();
    auto const freeRegion = std::next(slots.begin(), static_cast<ptrdiff_t>(unbox(_pageSize.lines)));
    if (newSlotCount > oldSlotCount)
        // The prototype rather than a default-constructed Line: a cols=0 line would violate the
        // "every slot is at least pageSize.columns wide" invariant the other paths rely on.
        slots.insert(freeRegion,
                     newSlotCount - oldSlotCount,
                     Line(_pageSize.columns, defaultLineFlags(), GraphicsAttributes {}));
    else if (newSlotCount < oldSlotCount)
        slots.erase(freeRegion, std::next(freeRegion, static_cast<ptrdiff_t>(oldSlotCount - newSlotCount)));

    _linesUsed = min(_linesUsed, newTotalLineCount);
    bumpGeneration(RowIdentity::Destroyed);
    verifyState();
}

void Grid::finalizeRevisions() noexcept
{
    // Scan the page, the prefix that scrolled out since the last finalize, and any history row
    // handed out for writing since then — all clamped to the valid history. The first covers rows
    // written and then scrolled away within one batch (stamped at their new negative offset); the
    // second covers rows dirtied in place with nothing scrolling at all (@see _dirtyHistoryFloor).
    auto const scrolledFloor = _stableBase - scrolledOutDepthSince(_stableBaseAtLastFinalize);
    auto const next = _seqno + 1;
    auto stamped = false;
    auto stampedHistoryFloor = NoHistoryFloor;
    for (auto offset = scanTopFor(std::min(scrolledFloor, _dirtyHistoryFloor));
         offset < boxed_cast<LineOffset>(_pageSize.lines);
         ++offset)
    {
        // rowAt, not lineAt: the mutable overload would re-arm the very floor this pass clears.
        if (!rowAt(offset).stampRevision(next))
            continue;
        stamped = true;
        if (offset < LineOffset(0))
            stampedHistoryFloor = std::min(stampedHistoryFloor, stableLineIdOf(offset));
    }
    if (stamped)
        _seqno = next;
    // Remember how deep a history change reached and when, so a consumer that has not caught up to
    // this batch still scans down to it (@see forEachLineChangedSince).
    //
    // Derived from the rows actually STAMPED, deliberately not from _dirtyHistoryFloor: that floor
    // is armed by handing a row out mutably, which a read-only caller sitting in a non-const
    // function does too (Screen::captureBuffer walks the whole scrollback). Keying the sticky
    // watermark on it would let one buffer capture pin every later delta scan at the top of the
    // history for the rest of the generation. The per-row branch below costs one predictable
    // compare and is paid only for rows that were genuinely dirty.
    if (stampedHistoryFloor != NoHistoryFloor)
    {
        _changedHistoryFloor = std::min(_changedHistoryFloor, stampedHistoryFloor);
        _changedHistorySeqno = next;
    }
    _dirtyHistoryFloor = NoHistoryFloor;
    _stableBaseAtLastFinalize = _stableBase;
}

void Grid::clearHistory()
{
    _linesUsed = _pageSize.lines;
    // The floor jumps to the base: every history id is evicted, no resend needed —
    // deliberately NOT a generation bump (page row identity is untouched).
    syncStableFloor();
    verifyState();
}

void Grid::verifyState() const noexcept
{
#ifdef CONTOUR_VERIFY_STATE
    Require(LineCount::cast_from(_lines.size()) >= totalLineCount());
    Require(LineCount::cast_from(_lines.size()) >= _linesUsed);
    Require(_linesUsed >= _pageSize.lines);
    // A floor below base - history would re-validate evicted stable ids.
    Require(_stableFloor >= _stableBase - unbox<int64_t>(historyLineCount()));
    // A floor above the base would deny live page rows their identity (and hand
    // std::clamp an inverted range); rotateBuffersRight re-keys before that.
    Require(_stableFloor <= _stableBase);
#endif
}

std::string Grid::renderAllText() const
{
    std::string text;
    text.reserve(unbox<size_t>(historyLineCount() + _pageSize.lines) * unbox<size_t>(_pageSize.columns + 1));

    for (auto y = LineOffset(0); y < LineOffset::cast_from(_lines.size()); ++y)
    {
        text += lineText(y);
        text += '\n';
    }

    return text;
}

std::string Grid::renderMainPageText() const
{
    std::string text;

    for (auto line = LineOffset(0); line < unbox<LineOffset>(_pageSize.lines); ++line)
    {
        text += lineText(line);
        text += '\n';
    }

    return text;
}

std::vector<std::string> Grid::renderRange(LineOffset start,
                                           LineOffset end,
                                           CaptureRendition rendition,
                                           CaptureTrailingSpaces trailing) const
{
    // The same top forEachValidLine walks from, and for the same reason: at capacity, scrollDown
    // wraps destroyed page rows into the oldest history slots without resetting them, so the rows
    // between -historyLineCount() and the stable floor hold garbage a capture must not return.
    auto const top = addressableTop();
    auto const bottom = unbox<LineOffset>(_pageSize.lines) - LineOffset(1);
    auto const from = std::clamp(start, top, bottom);
    auto const to = std::clamp(end, top, bottom);

    auto lines = std::vector<std::string> {};
    if (from > to)
        return lines;
    lines.reserve(unbox<size_t>(to - from) + 1);
    for (auto y = from; y <= to; ++y)
    {
        auto const& line = lineAt(y);
        auto const columns = trailing == CaptureTrailingSpaces::Keep
                                 ? unbox<ColumnOffset>(_pageSize.columns)
                                 : unbox<ColumnOffset>(line.trimmedColumns());
        lines.push_back(rendition == CaptureRendition::WithSgr ? line.toUtf8WithSgr(ColumnOffset(0), columns)
                                                               : line.toUtf8(ColumnOffset(0), columns));
    }
    return lines;
}

Line& Grid::lineAt(LineOffset line) noexcept
{
    return rowAt(line);
}

Line const& Grid::lineAt(LineOffset line) const noexcept
{
    return const_cast<Grid&>(*this).rowAt(line);
}

CellProxy Grid::at(LineOffset line, ColumnOffset column) noexcept
{
    return useCellAt(line, column);
}

CellProxy Grid::useCellAt(LineOffset line, ColumnOffset column) noexcept
{
    return lineAt(line).useCellAt(column);
}

CellProxy Grid::at(LineOffset line, ColumnOffset column) const noexcept
{
    return const_cast<Grid&>(*this).at(line, column);
}

void Grid::resetPageLines(LineCount count, GraphicsAttributes defaultAttributes) noexcept
{
    for (auto const line: std::views::iota(0, *count))
        lineAt(LineOffset(line)).reset(defaultLineFlags(), defaultAttributes);
}

// }}}
// {{{ Grid impl: Line access
std::string Grid::lineText(LineOffset lineOffset) const
{
    return lineAt(lineOffset).toUtf8();
}

std::string Grid::lineTextTrimmed(LineOffset lineOffset) const
{
    std::string output = lineText(lineOffset);
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

std::string Grid::lineText(Line const& line) const
{
    return line.toUtf8();
}

void Grid::setLineText(LineOffset line, std::string_view text)
{
    size_t i = 0;
    for (auto const ch: unicode::convert_to<char32_t>(text))
        useCellAt(line, ColumnOffset::cast_from(i++)).setCharacter(ch);
}

bool Grid::isLineBlank(LineOffset line) const noexcept
{
    return lineAt(line).empty();
}

LineOffset Grid::logicalLineHead(LineOffset line) const noexcept
{
    auto const historyTop = -boxed_cast<LineOffset>(historyLineCount());
    while (line > historyTop && lineAt(line).wrapped())
        --line;
    return line;
}

ColumnOffset Grid::logicalColumnOf(CellLocation position) const noexcept
{
    auto const head = logicalLineHead(position.line);
    auto const wrappedLinesAbove = *position.line - *head;
    return ColumnOffset::cast_from(wrappedLinesAbove * *_pageSize.columns) + position.column;
}

/**
 * Computes the relative line number for the bottom-most @p n logical lines.
 */
int Grid::computeLogicalLineNumberFromBottom(LineCount n) const noexcept
{
    int logicalLineCount = 0;
    auto outputRelativePhysicalLine = *_pageSize.lines - 1;

    auto i = _lines.rbegin();
    while (i != _lines.rend())
    {
        if (!i->wrapped())
            logicalLineCount++;
        outputRelativePhysicalLine--;
        ++i;
        if (logicalLineCount == *n)
            break;
    }

    while (i != _lines.rend() && i->wrapped())
    {
        outputRelativePhysicalLine--;
        ++i;
    }

    return outputRelativePhysicalLine;
}
// }}}
// {{{ Grid impl: scrolling
LineCount Grid::scrollUp(LineCount linesCountToScrollUp, GraphicsAttributes defaultAttributes) noexcept
{
    verifyState();

    // Before the branching, not after: the branch below that grows the history also contains the
    // at-capacity path in its tail, so one call can both fill the last free slot and start
    // recycling. Trimming first is also what hands that path a correctly positioned free region --
    // at capacity the free slots begin at exactly _pageSize.lines, which is where it appends.
    if (historyLineCount() + linesCountToScrollUp > maxHistoryLineCount())
        clampHistory();

    auto const linesAvailable = LineCount::cast_from(_lines.size() - unbox<size_t>(_linesUsed));
    if (std::holds_alternative<Infinite>(_historyLimits.capacity) && linesAvailable < linesCountToScrollUp)
    {
        auto const linesToAllocate = unbox(linesCountToScrollUp - linesAvailable);

        for ([[maybe_unused]] auto const _: std::views::iota(0, linesToAllocate))
            _lines.emplace_back(_pageSize.columns, defaultLineFlags(), GraphicsAttributes());

        return scrollUp(linesCountToScrollUp, defaultAttributes);
    }
    if (unbox<size_t>(_linesUsed) == _lines.size())
    {
        rotateBuffersLeft(linesCountToScrollUp);

        for (auto y = boxed_cast<LineOffset>(_pageSize.lines - linesCountToScrollUp);
             y < boxed_cast<LineOffset>(_pageSize.lines);
             ++y)
            lineAt(y).reset(defaultLineFlags(), defaultAttributes);

        return linesCountToScrollUp;
    }
    else
    {
        Require(unbox<size_t>(_linesUsed) < _lines.size());

        auto const linesAppendCount = std::min(linesCountToScrollUp, linesAvailable);

        if (*linesAppendCount != 0)
        {
            _linesUsed += linesAppendCount;
            syncStableFloor();
            Require(unbox<size_t>(_linesUsed) <= _lines.size());
            fill_n(next(_lines.begin(), *_pageSize.lines),
                   unbox<size_t>(linesAppendCount),
                   Line(_pageSize.columns, defaultLineFlags(), defaultAttributes));
            rotateBuffersLeft(linesAppendCount);
        }
        if (linesAppendCount < linesCountToScrollUp)
        {
            auto const incrementCount = linesCountToScrollUp - linesAppendCount;
            rotateBuffersLeft(incrementCount);

            for (auto y = boxed_cast<LineOffset>(_pageSize.lines - linesCountToScrollUp);
                 y < boxed_cast<LineOffset>(_pageSize.lines);
                 ++y)
                lineAt(y).reset(defaultLineFlags(), defaultAttributes);
        }
        return LineCount::cast_from(linesAppendCount);
    }
}

LineCount Grid::scrollUp(LineCount n, GraphicsAttributes defaultAttributes, Margin margin) noexcept
{
    verifyState();
    Require(0 <= *margin.horizontal.from && *margin.horizontal.to < *_pageSize.columns);
    Require(0 <= *margin.vertical.from && *margin.vertical.to < *_pageSize.lines);

    auto const fullHorizontal = margin.horizontal
                                == Margin::Horizontal { .from = ColumnOffset { 0 },
                                                        .to = unbox<ColumnOffset>(_pageSize.columns) - 1 };
    auto const fullVertical =
        margin.vertical
        == Margin::Vertical { .from = LineOffset(0), .to = unbox<LineOffset>(_pageSize.lines) - 1 };

    if (fullHorizontal)
    {
        if (fullVertical)
            return scrollUp(n, defaultAttributes);

        auto const marginHeight = LineCount(margin.vertical.length());
        auto const n2 = std::min(n, marginHeight);
        if (*n2 == 0)
            return LineCount(0);

        if (*margin.vertical.from == 0)
        {
            auto const historyLinesAdded = scrollUp(n2, defaultAttributes);
            auto const firstProtectedLine = margin.vertical.to + 1;
            auto const pageBottomLine = boxed_cast<LineOffset>(_pageSize.lines) - LineOffset(1);

            for (auto targetLine = pageBottomLine; targetLine >= firstProtectedLine; --targetLine)
            {
                auto const sourceLine = targetLine - *n2;
                lineAt(targetLine) = std::move(lineAt(sourceLine));
            }

            // Blank the new region rows at the region bottom.
            for (auto const lineNumber:
                 std::views::iota(*margin.vertical.to - *n2 + 1, *margin.vertical.to + 1))
                lineAt(LineOffset(lineNumber))
                    .reset(defaultLineFlags(), defaultAttributes, _pageSize.columns);

            verifyState();
            return historyLinesAdded;
        }

        if (*n2 && n2 < marginHeight)
        {
            for (auto topLineOffset = *margin.vertical.from; topLineOffset <= *margin.vertical.to - *n2;
                 ++topLineOffset)
                _lines[topLineOffset] = std::move(_lines[topLineOffset + *n2]);
        }

        auto const topEmptyLineNr = *margin.vertical.to - *n2 + 1;
        auto const bottomLineNumber = *margin.vertical.to;
        for (auto lineNumber = topEmptyLineNr; lineNumber <= bottomLineNumber; ++lineNumber)
        {
            _lines[lineNumber].reset(defaultLineFlags(), defaultAttributes, _pageSize.columns);
        }
    }
    else
    {
        auto const marginHeight = margin.vertical.length();
        auto const n2 = std::min(n, marginHeight);
        auto const topTargetLineOffset = margin.vertical.from;
        auto const bottomTargetLineOffset = margin.vertical.to - *n2;
        auto const columnsToMove = unbox<size_t>(margin.horizontal.length());

        for (LineOffset targetLineOffset = topTargetLineOffset; targetLineOffset <= bottomTargetLineOffset;
             ++targetLineOffset)
        {
            auto const sourceLineOffset = targetLineOffset + *n2;
            auto& targetLine = lineAt(targetLineOffset);
            auto const& sourceLine = lineAt(sourceLineOffset);
            auto const fromCol = unbox<size_t>(margin.horizontal.from);
            // copyColumns handles a blank source by clearing the destination range.
            // The destination needs to be materialized for the write — but only when the
            // copy would actually change the destination. Two blank lines with the same
            // fillAttrs collapse to a no-op; differing fillAttrs require materialization
            // so the source's attrs propagate into the target's [fromCol, fromCol+count).
            // Through std::as_const: the mutable storage() overload dirties pessimistically, and
            // this branch reads the target purely to decide that it needs no write at all. Taking
            // the mutable overload here stamps a provably unchanged row into the next delta batch,
            // once per skipped row per scroll.
            if (sourceLine.isBlank() && targetLine.isBlank()
                && sourceLine.storage().fillAttrs == std::as_const(targetLine).storage().fillAttrs)
                continue;
            copyColumns(
                sourceLine.storage(), fromCol, targetLine.materializedStorage(), fromCol, columnsToMove);
        }

        for (LineOffset line = margin.vertical.to - *n2 + 1; line <= margin.vertical.to; ++line)
        {
            auto& targetLine = lineAt(line);
            auto const fromCol = unbox<size_t>(margin.horizontal.from);
            // Clearing a blank line with non-default attrs would change semantics; only skip
            // when the target is already blank with matching fillAttrs.
            if (targetLine.isBlankWithFillAttrs(defaultAttributes))
                continue;
            clearRange(targetLine.materializedStorage(),
                       fromCol,
                       unbox<size_t>(margin.horizontal.length()),
                       defaultAttributes);
        }
    }
    verifyState();
    return LineCount(0);
}

void Grid::scrollDown(LineCount vN, GraphicsAttributes const& defaultAttributes, Margin const& margin)
{
    verifyState();
    Require(vN >= LineCount(0));

    auto const fullHorizontal =
        margin.horizontal
        == Margin::Horizontal { .from = ColumnOffset { 0 },
                                .to = unbox<ColumnOffset>(_pageSize.columns) - ColumnOffset(1) };
    auto const fullVertical =
        margin.vertical
        == Margin::Vertical { .from = LineOffset(0),
                              .to = unbox<LineOffset>(_pageSize.lines) - LineOffset(1) };

    auto const n = std::min(vN, margin.vertical.length());

    if (fullHorizontal && fullVertical)
    {
        rotateBuffersRight(n);
        resetPageLines(n, defaultAttributes);
        return;
    }

    if (fullHorizontal)
    {
        auto a = std::next(begin(_lines), *margin.vertical.from);
        auto b = std::next(begin(_lines), *margin.vertical.to + 1 - *n);
        auto c = std::next(begin(_lines), *margin.vertical.to + 1);
        std::rotate(a, b, c);
        for (auto const i: std::views::iota(*margin.vertical.from, *margin.vertical.from + *n))
            _lines[i].reset(defaultLineFlags(), defaultAttributes);
    }
    else
    {
        if (n <= margin.vertical.length())
        {
            // Shift every line that has somewhere to go: targets [from+n, to], each pulling from n rows
            // above. The lower bound is from+n, not to-n -- with a full-height vertical margin the latter
            // only touches the bottom n+1 (often blank) rows and never moves the content down at all.
            // Iterate downward so each source is read before it is overwritten as a later target.
            for (LineOffset line = margin.vertical.to; line >= margin.vertical.from + *n; --line)
            {
                auto const& srcLine = lineAt(line - *n);
                auto& dstLine = lineAt(line);
                auto const fromCol = unbox<size_t>(margin.horizontal.from);
                // Only skip when both lines are blank AND share fillAttrs; differing
                // attrs must propagate into the copied range (requires materialization).
                // std::as_const for the same reason as in scrollUp: the read that decides "no write
                // needed" must not go through the dirtying overload.
                if (srcLine.isBlank() && dstLine.isBlank()
                    && srcLine.storage().fillAttrs == std::as_const(dstLine).storage().fillAttrs)
                    continue;
                copyColumns(srcLine.storage(),
                            fromCol,
                            dstLine.materializedStorage(),
                            fromCol,
                            unbox<size_t>(margin.horizontal.length()));
            }

            for (LineOffset line = margin.vertical.from; line < margin.vertical.from + *n; ++line)
            {
                auto& targetLine = lineAt(line);
                auto const fromCol = unbox<size_t>(margin.horizontal.from);
                if (targetLine.isBlankWithFillAttrs(defaultAttributes))
                    continue;
                clearRange(targetLine.materializedStorage(),
                           fromCol,
                           unbox<size_t>(margin.horizontal.length()),
                           defaultAttributes);
            }
        }
    }
}

void Grid::unscroll(LineCount n, GraphicsAttributes const& defaultAttributes)
{
    verifyState();
    auto const clampedN = std::min(n, _pageSize.lines);
    auto const pullable = std::min(clampedN, historyLineCount());

    if (*pullable > 0)
    {
        rotateBuffersRight(pullable);
        _linesUsed -= pullable;
        syncStableFloor();
    }

    auto const remaining = clampedN - pullable;
    if (*remaining > 0)
    {
        rotateBuffersRight(remaining);
        resetPageLines(remaining, defaultAttributes);
    }
    verifyState();
}

void Grid::scrollLeft(GraphicsAttributes defaultAttributes, Margin margin) noexcept
{
    for (LineOffset lineNo = margin.vertical.from; lineNo <= margin.vertical.to; ++lineNo)
    {
        auto& line = lineAt(lineNo);
        auto const from = unbox<size_t>(margin.horizontal.from);
        auto const to = unbox<size_t>(margin.horizontal.to) + 1;
        auto const count = to - from;
        if (count > 1)
        {
            // Blank line with matching fillAttrs is invariant under this rotation.
            if (line.isBlankWithFillAttrs(defaultAttributes))
                continue;
            auto& storage = line.materializedStorage();
            moveColumns(storage, from + 1, from, count - 1);
            clearRange(storage, from + count - 1, 1, defaultAttributes);
        }
    }
}

// }}}
// {{{ Grid impl: resize
void Grid::reset()
{
    _linesUsed = _pageSize.lines;
    _lines.rotateRight(_lines.zeroIndex());
    for (int i = 0; i < unbox(_pageSize.lines); ++i)
        _lines[i].reset(defaultLineFlags(), GraphicsAttributes {});
    bumpGeneration(RowIdentity::Destroyed);
    verifyState();
}

CellLocation Grid::growLines(LineCount newHeight, CellLocation cursor)
{
    Require(newHeight > _pageSize.lines);

    CellLocation cursorMove {};
    if (*cursor.line + 1 == *_pageSize.lines)
    {
        auto const totalLinesToExtend = newHeight - _pageSize.lines;
        auto const linesToTakeFromSavedLines = std::min(totalLinesToExtend, historyLineCount());
        Require(totalLinesToExtend >= linesToTakeFromSavedLines);
        Require(*linesToTakeFromSavedLines >= 0);
        rotateBuffersRight(linesToTakeFromSavedLines);
        _pageSize.lines += linesToTakeFromSavedLines;
        cursorMove.line += boxed_cast<LineOffset>(linesToTakeFromSavedLines);
    }

    auto const wrappableFlag = _lines.back().wrappableFlag();
    auto const totalLinesToExtend = newHeight - _pageSize.lines;
    Require(*totalLinesToExtend >= 0);

    auto const newTotalLineCount = maxHistoryLineCount() + newHeight;
    auto const currentTotalLineCount = LineCount::cast_from(_lines.size());
    auto const linesToFill = max(0, *newTotalLineCount - *currentTotalLineCount);

    for ([[maybe_unused]] auto const _: std::views::iota(0, linesToFill))
        _lines.emplace_back(_pageSize.columns, wrappableFlag, GraphicsAttributes {});

    _pageSize.lines += totalLinesToExtend;
    _linesUsed = min(_linesUsed + totalLinesToExtend, LineCount::cast_from(_lines.size()));
    syncStableFloor(); // the min() clamp can shrink the history at ring capacity

    Ensures(_pageSize.lines == newHeight);
    Ensures(_lines.size() >= unbox<size_t>(maxHistoryLineCount() + _pageSize.lines));
    verifyState();

    return cursorMove;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
CellLocation Grid::resize(PageSize newSize, CellLocation currentCursorPos, bool wrapPending)
{
    if (_pageSize == newSize)
        return currentCursorPos;

    gridLog()("resize {} -> {} (cursor {})", _pageSize, newSize, currentCursorPos);

    // {{{ helper methods
    auto const shrinkLines = [this](LineCount newHeight, CellLocation cursor) -> CellLocation {
        Require(newHeight < _pageSize.lines);

        auto const numLinesToShrink = _pageSize.lines - newHeight;
        auto const linesAvailableBelowCursorBeforeShrink =
            _pageSize.lines - boxed_cast<LineCount>(cursor.line + 1);
        auto const cutoffCount = min(numLinesToShrink, linesAvailableBelowCursorBeforeShrink);
        auto const numLinesToPushUp = numLinesToShrink - cutoffCount;
        auto const numLinesToPushUpCapped = min(numLinesToPushUp, maxHistoryLineCount());

        gridLog()(" -> shrink lines: numLinesToShrink {}, linesAvailableBelowCursorBeforeShrink {}, "
                  "cutoff {}, pushUp "
                  "{}/{}",
                  numLinesToShrink,
                  linesAvailableBelowCursorBeforeShrink,
                  cutoffCount,
                  numLinesToPushUp,
                  numLinesToPushUpCapped);

        Ensures(numLinesToShrink == cutoffCount + numLinesToPushUp);

        if (cutoffCount != LineCount(0))
        {
            _pageSize.lines -= cutoffCount;
            _linesUsed -= cutoffCount;
            syncStableFloor();
            Ensures(*cursor.line < *_pageSize.lines);
            verifyState();
        }

        Require(newHeight <= _pageSize.lines);
        if (*numLinesToPushUp)
        {
            gridLog()(" -> numLinesToPushUp {}", numLinesToPushUp);
            Require(*cursor.line + 1 == *_pageSize.lines);
            rotateBuffersLeft(numLinesToPushUp);
            _pageSize.lines -= numLinesToPushUp;
            clampHistory();
            verifyState();
            return CellLocation { .line = -boxed_cast<LineOffset>(numLinesToPushUp), .column = {} };
        }

        verifyState();
        return CellLocation {};
    };

    auto const growColumns = [this, wrapPending](ColumnCount newColumnCount) -> CellLocation {
        if (!_reflowOnResize)
        {
            for (auto& line: _lines)
                if (line.size() < newColumnCount)
                    line.resize(newColumnCount);
            _pageSize.columns = newColumnCount;
            verifyState();
            return CellLocation { .line = LineOffset(0), .column = ColumnOffset(wrapPending ? 1 : 0) };
        }
        else
        {
            auto const extendCount = newColumnCount - _pageSize.columns;
            Require(*extendCount > 0);

            Lines grownLines;
            LineSoA logicalLineBuffer;
            initializeLineSoA(logicalLineBuffer, ColumnCount(0));
            size_t logicalLineUsed = 0;
            auto logicalLineAttributes = detail::LogicalLineAttributes {};

            auto const appendToLogicalLine = [&logicalLineBuffer, &logicalLineUsed](Line const& line) {
                auto const cols = unbox<size_t>(line.size());
                auto const used = trimBlankRight(line.storage(), cols);
                if (used > 0)
                {
                    auto const newSize = logicalLineUsed + used;
                    resizeLineSoA(logicalLineBuffer, ColumnCount::cast_from(newSize));
                    copyColumns(line.storage(), 0, logicalLineBuffer, logicalLineUsed, used);
                    logicalLineUsed = newSize;
                }
            };

            auto const flushLogicalLine = [newColumnCount,
                                           &grownLines,
                                           &logicalLineBuffer,
                                           &logicalLineUsed,
                                           &logicalLineAttributes]() {
                if (logicalLineUsed > 0)
                {
                    detail::addNewWrappedLines(grownLines,
                                               newColumnCount,
                                               logicalLineBuffer,
                                               logicalLineUsed,
                                               logicalLineAttributes,
                                               true);
                    initializeLineSoA(logicalLineBuffer, ColumnCount(0));
                    logicalLineUsed = 0;
                }
            };

            for (int i = -*historyLineCount(); i < *_pageSize.lines; ++i)
            {
                auto& line = _lines[i];
                Require(line.size() >= _pageSize.columns);

                if (line.wrapped())
                {
                    appendToLogicalLine(line);
                }
                else
                {
                    flushLogicalLine();
                    if (line.empty())
                    {
                        line.resize(newColumnCount);
                        grownLines.emplace_back(std::move(line));
                    }
                    else
                    {
                        appendToLogicalLine(line);
                        logicalLineAttributes = {
                            .flags = line.flags().without(LineFlag::Wrapped),
                            .commandEndOffset = line.commandEndOffset(),
                            .promptEndOffset = line.promptEndOffset(),
                            .contextId = line.contextId(),
                        };
                    }
                }
            }

            flushLogicalLine();

            auto cy = LineCount(0);
            if (_pageSize.lines > LineCount::cast_from(grownLines.size()))
            {
                cy = _pageSize.lines - LineCount::cast_from(grownLines.size());
                while (LineCount::cast_from(grownLines.size()) < _pageSize.lines)
                    grownLines.emplace_back(newColumnCount, defaultLineFlags(), GraphicsAttributes {});

                Ensures(LineCount::cast_from(grownLines.size()) == _pageSize.lines);
            }

            _linesUsed = LineCount::cast_from(grownLines.size());

            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            while (grownLines.size() < totalLineCount)
                grownLines.emplace_back(newColumnCount, defaultLineFlags(), GraphicsAttributes {});

            _lines = std::move(grownLines);
            _pageSize.columns = newColumnCount;

            auto const newHistoryLineCount = _linesUsed - _pageSize.lines;
            rotateBuffersLeft(newHistoryLineCount);

            verifyState();
            return CellLocation { .line = -boxed_cast<LineOffset>(cy),
                                  .column = ColumnOffset(wrapPending ? 1 : 0) };
        }
    };

    auto const shrinkColumns =
        [this](ColumnCount newColumnCount, LineCount /*newLineCount*/, CellLocation cursor) -> CellLocation {
        if (!_reflowOnResize)
        {
            _pageSize.columns = newColumnCount;
            for (auto& line: _lines)
                if (newColumnCount < line.size())
                    line.resize(newColumnCount);
            verifyState();
            return cursor + std::min(cursor.column, boxed_cast<ColumnOffset>(newColumnCount));
        }
        else
        {
            Lines shrunkLines;
            LineSoA wrappedColumns;
            initializeLineSoA(wrappedColumns, ColumnCount(0));
            size_t wrappedUsed = 0;
            // A run of physical rows being rejoined into one logical line was all written by one
            // context, so the chunks that run is re-split into all belong to it -- which is why the
            // context travels beside the flags rather than being defaulted per chunk. The two offsets
            // stay at zero here: a shrink emits only continuations, which carry neither.
            auto previousAttributes =
                detail::LogicalLineAttributes { .flags = _lines.front().inheritableFlags(),
                                                .contextId = _lines.front().contextId() };

            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            shrunkLines.reserve(totalLineCount);
            Require(totalLineCount == unbox<size_t>(this->totalLineCount()));

            auto numLinesWritten = LineCount(0);
            for (auto i = -*historyLineCount(); i < *_pageSize.lines; ++i)
            {
                auto& line = _lines[i];

                if (wrappedUsed > 0)
                {
                    if (line.wrapped() && line.inheritableFlags() == previousAttributes.flags)
                    {
                        // Prepend wrapped columns to this line using SoA copyColumns
                        auto const cols = unbox<size_t>(line.size());
                        auto const used = trimBlankRight(line.storage(), cols);
                        auto const totalCols = wrappedUsed + used;
                        LineSoA merged;
                        initializeLineSoA(merged, ColumnCount::cast_from(totalCols));
                        copyColumns(wrappedColumns, 0, merged, 0, wrappedUsed);
                        if (used > 0)
                            copyColumns(line.storage(), 0, merged, wrappedUsed, used);

                        // The one place a Line is rebuilt from its parts. It is a CONTINUATION (the branch
                        // is guarded on it), so it carries no command-end offset to lose — but say so out
                        // loud rather than let the constructor's default quietly stand in for the reason.
                        Require(line.commandEndOffset() == ColumnOffset(0));
                        // Carried across the rebuild explicitly. Unlike the offsets above, the context
                        // is NOT default-correct here: this line and the columns being prepended to it
                        // are two physical pieces of ONE logical line, so they share an author, and a
                        // fresh Line would silently report none.
                        auto const carriedContext = line.contextId();
                        line = Line(line.flags(), std::move(merged), ColumnCount::cast_from(totalCols));
                        line.adoptContext(carriedContext);
                    }
                    else
                    {
                        // Every chunk here is the OVERFLOW of a head that was already emitted, so it is a
                        // continuation: no semantic marks, and therefore no command-end offset either.
                        auto const numLinesInserted = detail::addNewWrappedLines(shrunkLines,
                                                                                 newColumnCount,
                                                                                 wrappedColumns,
                                                                                 wrappedUsed,
                                                                                 previousAttributes,
                                                                                 false);
                        numLinesWritten += numLinesInserted;
                        previousAttributes = { .flags = line.inheritableFlags(),
                                               .contextId = line.contextId() };
                    }
                    wrappedUsed = 0;
                    initializeLineSoA(wrappedColumns, ColumnCount(0));
                }
                else
                {
                    line.setWrappable(true);
                    previousAttributes = { .flags = line.inheritableFlags(), .contextId = line.contextId() };
                }

                auto overflow = line.reflow(newColumnCount);
                auto const overflowCols = overflow.codepoints.size();
                if (overflowCols > 0)
                {
                    wrappedColumns = std::move(overflow);
                    wrappedUsed = trimBlankRight(wrappedColumns, overflowCols);
                }

                shrunkLines.emplace_back(std::move(line));
                numLinesWritten++;
                Ensures(shrunkLines.back().size() >= newColumnCount);
            }
            if (wrappedUsed > 0)
            {
                numLinesWritten += detail::addNewWrappedLines(
                    shrunkLines, newColumnCount, wrappedColumns, wrappedUsed, previousAttributes, false);
            }
            Require(unbox<size_t>(numLinesWritten) == shrunkLines.size());
            Require(numLinesWritten >= _pageSize.lines);

            while (shrunkLines.size() < totalLineCount)
                shrunkLines.emplace_back(newColumnCount, LineFlag::None, GraphicsAttributes {});

            _linesUsed = LineCount::cast_from(numLinesWritten);
            _lines = std::move(shrunkLines);
            _pageSize.columns = newColumnCount;

            // The rotation goes through the ring primitive rather than being applied to the local
            // vector, because rotateBuffersLeft is where the stable-id accounting lives. Rotating
            // `shrunkLines` instead left `_stableBase` exactly where it was while the history GREW
            // underneath it — and syncStableFloor()'s max() can only RAISE the floor, never lower
            // one — so every history row the reflow had just created sat below `_stableFloor` and
            // forEachValidLine() began its walk above them: a client attaching after a narrowing
            // resize got a grid with no scrollback at all, though the daemon still held the rows.
            // growColumns has always gone through this primitive; the two paths now agree.
            rotateBuffersLeft(historyLineCount());

            verifyState();
            return cursor; // TODO
        }
    };
    // }}}

    CellLocation cursor = currentCursorPos;

    // Read before either resize below moves it: only a COLUMN change reflows, and reflow is the half
    // of this that rebuilds the ring and with it every row's identity.
    auto const columnsChanged = newSize.columns != _pageSize.columns;

    using crispy::Comparison;
    switch (crispy::strongCompare(newSize.columns, _pageSize.columns))
    {
        case Comparison::Greater: cursor += growColumns(newSize.columns); break;
        case Comparison::Less: cursor = shrinkColumns(newSize.columns, newSize.lines, cursor); break;
        case Comparison::Equal: break;
    }

    switch (crispy::strongCompare(newSize.lines, _pageSize.lines))
    {
        case Comparison::Greater: cursor += growLines(newSize.lines, cursor); break;
        case Comparison::Less: cursor += shrinkLines(newSize.lines, cursor); break;
        case Comparison::Equal: break;
    }

    Ensures(_pageSize == newSize);
    // Any page-size change makes a mirror resync, so the generation bumps either way. Stable ids are
    // the narrower question: growLines/shrinkLines rotate through the ring primitives that carry the
    // id accounting, so a pure LINE-count change leaves every row naming what it named -- which is
    // what lets a collapsed fold survive a status line appearing or a window growing taller. A column
    // change is the one that rebuilds the ring.
    bumpGeneration(columnsChanged ? RowIdentity::Destroyed : RowIdentity::Preserved);
    verifyState();

    return cursor;
}

void Grid::clampHistory()
{
    // No headroom means no trade to make: the scrollback evicts line-wise, exactly as it always has.
    if (!_historyLimits.hasHeadroom())
        return;

    // An unbounded scrollback evicts nothing, so there is no eviction to snap to a boundary. Without
    // this the ring's current allocation would read as the capacity and the trim would start cutting
    // the very history `infinite` promises to keep.
    if (std::holds_alternative<Infinite>(_historyLimits.capacity))
        return;

    // A zero-history grid is every page but the primary one. Returning here also makes the
    // monotone-floor reasoning below unconditional rather than true-by-audit: rotateBuffersRight has
    // a zero-history branch that LOWERS _stableFloor, and it is the only path that ever does.
    auto const historyLineCount = this->historyLineCount();
    if (historyLineCount <= LineCount(0))
        return;

    auto const guaranteed = guaranteedHistoryLineCount();
    if (historyLineCount <= guaranteed)
        return;

    // Rows at offsets below `bound` may go; a block start AT bound leaves exactly the guarantee.
    // Clamped to -1 because offset 0 is the page, not the scrollback.
    auto const bound = std::min(LineOffset::cast_from(-unbox<int>(guaranteed)), LineOffset(-1));

    // The scan stops at addressableTop() rather than at -historyLineCount(): at capacity, scrollDown
    // wraps destroyed page rows into the oldest history slots WITHOUT resetting them, so those slots
    // keep whatever LineFlag::Marked they last held. Reading one as a block start would evict to a
    // prompt that is not there. @see addressableTop.
    auto const top = addressableTop();
    if (bound < top)
        return;

    auto blockStart = std::optional<LineOffset> {};
    for (auto y = bound; y >= top; --y)
    {
        if (lineAt(y).marked())
        {
            blockStart = y;
            break;
        }
    }

    if (!blockStart)
        return;

    // Measured from the real bottom of the ring's history rather than from addressableTop(), so any
    // unaddressable garbage below the floor is dropped along with the rest instead of being left to
    // occupy slots nothing can read.
    auto const dropCount = LineCount::cast_from(unbox<int>(*blockStart) + unbox<int>(historyLineCount));
    if (dropCount <= LineCount(0))
        return;

    // Reset before forgetting them: an inflated LineSoA would otherwise sit on its heap buffers until
    // the slot happened to be reused, and a stale Marked flag would be a phantom block start for the
    // next scan. rowAt rather than changingLineAt -- these rows are leaving, so announcing them to
    // the delta stream would arm a history watermark for content no client will ever ask for.
    for (auto y = LineOffset::cast_from(-unbox<int>(historyLineCount)); y < *blockStart; ++y)
        rowAt(y).reset(defaultLineFlags(), GraphicsAttributes {});

    // The whole eviction, in two lines: the ring keeps its slots and its rotation, the rows simply
    // stop being counted, and the freed slots rejoin the free region scrollUp appends into. No
    // generation bump -- every surviving row keeps the id it had, and the rising floor is the
    // signal consumers already watch for an eviction (@see FoldState::prune).
    _linesUsed -= dropCount;
    syncStableFloor();
    verifyState();
}
// }}}
// {{{ dumpGrid impl
std::ostream& dumpGrid(std::ostream& os, Grid const& grid)
{
    os << std::format(
        "main page lines: scrollback cur {} max {}, main page lines {}, used lines {}, zero index {}\n",
        grid.historyLineCount(),
        grid.maxHistoryLineCount(),
        grid.pageSize().lines,
        grid.linesUsed(),
        grid.zeroIndex());

    for (int const lineOffset:
         std::views::iota(-unbox(grid.historyLineCount()), unbox(grid.pageSize().lines)))
    {
        Line const& lineAttribs = grid.lineAt(LineOffset(lineOffset));

        os << std::format("[{:>2}] \"{}\" | {}\n",
                          lineOffset,
                          grid.lineText(LineOffset::cast_from(lineOffset)),
                          lineAttribs.flags());
    }

    return os;
}

std::string dumpGrid(Grid const& grid)
{
    std::stringstream sstr;
    dumpGrid(sstr, grid);
    return sstr.str();
}
// }}}

CellLocationRange Grid::wordRangeUnderCursor(CellLocation position,
                                             u32string_view wordDelimiters) const noexcept
{
    auto const left = [this, wordDelimiters, position]() {
        auto last = position;
        auto current = last;

        for (;;)
        {
            auto const wrapIntoPreviousLine =
                *current.column == 0 && *current.line > 0 && isLineWrapped(current.line);
            if (*current.column > 0)
                current.column--;
            else if (*current.line > 0 || wrapIntoPreviousLine)
            {
                current.line--;
                current.column = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
            }
            else
                break;

            if (cellEmptyOrContainsOneOf(current, wordDelimiters))
                break;
            last = current;
        }
        return last;
    }();

    auto const right = [this, wordDelimiters, position]() {
        auto last = position;
        auto current = last;

        for (;;)
        {
            if (*current.column == *pageSize().columns - 1 && *current.line + 1 < *pageSize().lines
                && isLineWrapped(current.line))
            {
                current.line++;
                current.column = ColumnOffset(0);
                current =
                    stretchedColumn(CellLocation { .line = current.line, .column = current.column + 1 });
            }

            if (*current.column + 1 < *pageSize().columns)
            {
                current =
                    stretchedColumn(CellLocation { .line = current.line, .column = current.column + 1 });
            }
            else if (*current.line + 1 < *pageSize().lines)
            {
                current.line++;
                current.column = ColumnOffset(0);
            }
            else
                break;

            if (cellEmptyOrContainsOneOf(current, wordDelimiters))
                break;
            last = current;
        }
        return last;
    }();

    return CellLocationRange { .first = left, .second = right };
}

bool Grid::cellEmptyOrContainsOneOf(CellLocation position, u32string_view delimiters) const noexcept
{
    position.column = min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    auto cell = at(position.line, position.column);
    return CellUtil::empty(cell) || delimiters.contains(cell.codepoint(0));
}

u32string Grid::extractText(CellLocationRange range) const noexcept
{
    if (range.first.line > range.second.line)
        std::swap(range.first, range.second);

    auto output = u32string {};

    assert(range.first.line == range.second.line);

    auto const rightMargin = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
    auto const numLines = LineCount::cast_from(range.second.line - range.first.line + 1);
    auto ranges = vector<ColumnRange> {};

    switch (numLines.value)
    {
        case 1:
            ranges.emplace_back(ColumnRange { .line = range.first.line,
                                              .fromColumn = range.first.column,
                                              .toColumn = min(range.second.column, rightMargin) });
            break;
        case 2:
            ranges.emplace_back(ColumnRange {
                .line = range.first.line, .fromColumn = range.first.column, .toColumn = rightMargin });
            ranges.emplace_back(ColumnRange { .line = range.first.line,
                                              .fromColumn = ColumnOffset(0),
                                              .toColumn = min(range.second.column, rightMargin) });
            break;
        default:
            ranges.emplace_back(ColumnRange {
                .line = range.first.line, .fromColumn = range.first.column, .toColumn = rightMargin });
            for (auto j = range.first.line + 1; j < range.second.line; ++j)
                ranges.emplace_back(
                    ColumnRange { .line = j, .fromColumn = ColumnOffset(0), .toColumn = rightMargin });
            ranges.emplace_back(ColumnRange { .line = range.second.line,
                                              .fromColumn = ColumnOffset(0),
                                              .toColumn = min(range.second.column, rightMargin) });
            break;
    }

    for (auto const& range: ranges)
    {
        if (!output.empty())
            output += '\n';
        for (auto column = range.fromColumn; column <= range.toColumn; ++column)
        {
            auto cell = at(range.line, column);
            if (cell.codepointCount() != 0)
                output += cell.codepoints();
            else
                output += L' ';
        }
    }

    return output;
}

} // end namespace vtbackend
