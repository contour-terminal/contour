/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vtbackend/Terminal.h>
#include <vtbackend/ViCommands.h>
#include <vtbackend/logging.h>

#include <fmt/format.h>

#include <memory>

namespace terminal
{

namespace
{
    CellLocation getRightMostNonEmptyCellLocation(Terminal const& terminal, LineOffset lineOffset) noexcept
    {
        if (terminal.isPrimaryScreen())
            return terminal.primaryScreen().grid().rightMostNonEmptyAt(lineOffset);
        else
            return terminal.alternateScreen().grid().rightMostNonEmptyAt(lineOffset);
    }

    constexpr std::optional<std::pair<char, bool>> matchingPairOfChar(char input) noexcept
    {
        auto constexpr pairs = std::array {
            std::pair { '(', ')' },
            std::pair { '[', ']' },
            std::pair { '{', '}' },
            std::pair { '<', '>' },
        };

        for (auto const& pair: pairs)
        {
            if (input == pair.first)
                return { { pair.second, true } };
            if (input == pair.second)
                return { { pair.first, false } };
        }

        return std::nullopt;
    }

} // namespace

using namespace std;

ViCommands::ViCommands(Terminal& theTerminal): _terminal { theTerminal }
{
}

void ViCommands::scrollViewport(ScrollOffset delta)
{
    if (delta.value < 0)
        _terminal.viewport().scrollDown(boxed_cast<LineCount>(-delta));
    else
        _terminal.viewport().scrollUp(boxed_cast<LineCount>(delta));
}

void ViCommands::searchStart()
{
    _terminal.state().searchMode.pattern.clear();
    _terminal.screenUpdated();
}

void ViCommands::searchDone()
{
    _terminal.screenUpdated();
}

void ViCommands::searchCancel()
{
    _terminal.state().searchMode.pattern.clear();
    _terminal.screenUpdated();
}

bool ViCommands::jumpToNextMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        auto startPosition = cursorPosition;
        if (startPosition.column < boxed_cast<ColumnOffset>(_terminal.pageSize().columns))
            startPosition.column++;
        else if (cursorPosition.line < boxed_cast<LineOffset>(_terminal.pageSize().lines) - 1)
        {
            startPosition.line++;
            startPosition.column = ColumnOffset(0);
        }

        auto const nextPosition = _terminal.search(startPosition);
        if (!nextPosition)
            return false;

        moveCursorTo(nextPosition.value());
    }
    return true;
}

bool ViCommands::jumpToPreviousMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        auto startPosition = cursorPosition;
        if (startPosition.column != ColumnOffset(0))
            startPosition.column--;
        else if (cursorPosition.line > -boxed_cast<LineOffset>(_terminal.currentScreen().historyLineCount()))
        {
            startPosition.line--;
            startPosition.column = boxed_cast<ColumnOffset>(_terminal.pageSize().columns) - 1;
        }

        auto const nextPosition = _terminal.searchReverse(startPosition);
        if (!nextPosition)
            return false;

        moveCursorTo(nextPosition.value());
    }
    return true;
}

void ViCommands::updateSearchTerm(std::u32string const& text)
{
    if (auto const newLocation = _terminal.searchReverse(text, cursorPosition))
        moveCursorTo(newLocation.value());
}

void ViCommands::modeChanged(ViMode mode)
{
    auto _ = crispy::finally { [this, mode]() {
        _lastMode = mode;
    } };

    InputLog()("mode changed to {}\n", mode);

    auto const selectFrom = _terminal.selector() ? _terminal.selector()->from() : cursorPosition;

    switch (mode)
    {
        case ViMode::Insert:
            // Force re-render as viewport & cursor might have changed.
            _terminal.setMode(DECMode::VisibleCursor, _lastCursorVisible);
            _terminal.setCursorShape(_lastCursorShape);
            _terminal.viewport().forceScrollToBottom();
            _terminal.popStatusDisplay();
            _terminal.screenUpdated();
            break;
        case ViMode::Normal:
            _lastCursorShape = _terminal.cursorShape();
            _lastCursorVisible = _terminal.isModeEnabled(DECMode::VisibleCursor);
            _terminal.setMode(DECMode::VisibleCursor, true);

            if (_lastMode == ViMode::Insert)
                cursorPosition = _terminal.currentScreen().cursor().position;
            if (_terminal.selectionAvailable())
                _terminal.clearSelection();
            _terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            _terminal.screenUpdated();
            break;
        case ViMode::Visual:
            _terminal.setSelector(make_unique<LinearSelection>(
                _terminal.selectionHelper(), selectFrom, _terminal.selectionUpdatedHelper()));
            _terminal.selector()->extend(cursorPosition);
            _terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            break;
        case ViMode::VisualLine:
            _terminal.setSelector(make_unique<FullLineSelection>(
                _terminal.selectionHelper(), selectFrom, _terminal.selectionUpdatedHelper()));
            _terminal.selector()->extend(cursorPosition);
            _terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            _terminal.screenUpdated();
            break;
        case ViMode::VisualBlock:
            _terminal.setSelector(make_unique<RectangularSelection>(
                _terminal.selectionHelper(), selectFrom, _terminal.selectionUpdatedHelper()));
            _terminal.selector()->extend(cursorPosition);
            _terminal.pushStatusDisplay(StatusDisplayType::Indicator);
            _terminal.screenUpdated();
            break;
    }

    _terminal.inputModeChanged(mode);
}

void ViCommands::reverseSearchCurrentWord()
{
    // auto const oldPos = cursorPosition;
    auto const [wordUnderCursor, range] = _terminal.extractWordUnderCursor(cursorPosition);
    assert(range.contains(cursorPosition));
    cursorPosition = range.first;

    updateSearchTerm(wordUnderCursor);
    jumpToPreviousMatch(1);
}

void ViCommands::searchCurrentWord()
{
    auto const [wordUnderCursor, range] = _terminal.extractWordUnderCursor(cursorPosition);
    assert(range.contains(cursorPosition));
    cursorPosition = range.second;
    updateSearchTerm(wordUnderCursor);
    jumpToNextMatch(1);
}

void ViCommands::executeYank(ViMotion motion, unsigned count)
{
    switch (motion)
    {
        case ViMotion::Selection: {
            assert(_terminal.selector());
            if (_lastMode == ViMode::VisualBlock)
                _terminal.setHighlightRange(
                    RectangularHighlight { _terminal.selector()->from(), _terminal.selector()->to() });
            else
                _terminal.setHighlightRange(
                    LinearHighlight { _terminal.selector()->from(), _terminal.selector()->to() });
            _terminal.copyToClipboard(_terminal.extractSelectionText());
            _terminal.inputHandler().setMode(ViMode::Normal);
            break;
        }
        default: {
            auto const [from, to] = translateToCellRange(motion, count);
            executeYank(from, to);
        }
        break;
    }
}

void ViCommands::executeYank(CellLocation from, CellLocation to)
{
    assert(_terminal.inputHandler().mode() == ViMode::Normal);
    assert(!_terminal.selector());

    // TODO: ideally keep that selection for about N msecs,
    // such that it'll be visually rendered and the user has a feedback of what's
    // being clipboarded.
    // Maybe via a event API to inform that a non-visual selection
    // has happened and that it can now either be instantly destroyed
    // or delayed (N msecs, configurable),
    _terminal.setSelector(
        make_unique<LinearSelection>(_terminal.selectionHelper(), from, _terminal.selectionUpdatedHelper()));
    _terminal.selector()->extend(to);
    auto const text = _terminal.extractSelectionText();
    _terminal.copyToClipboard(text);
    _terminal.clearSelection();
    _terminal.setHighlightRange(LinearHighlight { from, to });
    _terminal.inputHandler().setMode(ViMode::Normal);
    _terminal.screenUpdated();
}

void ViCommands::execute(ViOperator op, ViMotion motion, unsigned count)
{
    InputLog()("{}: Executing: {} {} {}\n", _terminal.inputHandler().mode(), count, op, motion);
    switch (op)
    {
        case ViOperator::MoveCursor:
            //.
            moveCursor(motion, count);
            break;
        case ViOperator::Yank:
            //.
            executeYank(motion, count);
            break;
        case ViOperator::Paste:
            //.
            _terminal.sendPasteFromClipboard(count, false);
            break;
        case ViOperator::PasteStripped:
            //.
            _terminal.sendPasteFromClipboard(count, true);
            break;
        case ViOperator::ReverseSearchCurrentWord: // TODO: Does this even make sense to have?
            break;
    }
    _terminal.screenUpdated();
}

void ViCommands::select(TextObjectScope scope, TextObject textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = to;
    InputLog()("{}: Executing: select {} {} [{} .. {}]\n",
               _terminal.inputHandler().mode(),
               scope,
               textObject,
               from,
               to);
    _terminal.setSelector(
        make_unique<LinearSelection>(_terminal.selectionHelper(), from, _terminal.selectionUpdatedHelper()));
    _terminal.selector()->extend(to);
    _terminal.screenUpdated();
}

void ViCommands::yank(TextObjectScope scope, TextObject textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = from;
    InputLog()("{}: Executing: yank {} {}\n", _terminal.inputHandler().mode(), scope, textObject);
    executeYank(from, to);
    _terminal.screenUpdated();
}

void ViCommands::yank(ViMotion motion)
{
    auto const [from, to] = translateToCellRange(motion, 1);
    cursorPosition = from;
    InputLog()("{}: Executing: motion-yank {}\n", _terminal.inputHandler().mode(), motion);
    executeYank(from, to);
    _terminal.screenUpdated();
}

void ViCommands::paste(unsigned count, bool stripped)
{
    _terminal.sendPasteFromClipboard(count, stripped);
}

CellLocation ViCommands::prev(CellLocation location) const noexcept
{
    if (location.column.value > 0)
        return { location.line, location.column - 1 };

    auto const rightMargin = _terminal.pageSize().columns.as<ColumnOffset>() - 1;
    auto const topLineOffset = _terminal.isPrimaryScreen()
                                   ? -boxed_cast<LineOffset>(_terminal.primaryScreen().historyLineCount()) + 1
                                   : LineOffset(0);
    if (location.line > topLineOffset)
    {
        location.line--;
        location.column = rightMargin;
    }

    return location;
}

CellLocation ViCommands::next(CellLocation location) const noexcept
{
    auto const rightMargin = _terminal.pageSize().columns.as<ColumnOffset>() - 1;
    if (location.column < rightMargin)
        return { location.line, location.column + 1 };

    if (location.line < boxed_cast<LineOffset>(_terminal.pageSize().lines))
    {
        location.line++;
        location.column = ColumnOffset(0);
    }

    return location;
}

CellLocation ViCommands::findMatchingPairFrom(CellLocation location) const noexcept
{
    auto const& cell = _terminal.primaryScreen().at(cursorPosition);
    if (cell.codepointCount() != 1)
        return location;

    auto const a = cell.codepoint(0);
    auto const matchResult = matchingPairOfChar(a);
    if (!matchResult)
        return location;
    auto const [b, left] = *matchResult;

    if (left)
        return findMatchingPairRight(a, b, 0);
    else
        return findMatchingPairLeft(b, a, 0);
}

CellLocation ViCommands::findMatchingPairLeft(char left, char right, int initialDepth) const noexcept
{
    auto a = cursorPosition;
    auto depth = initialDepth;

    while (true)
    {
        if (compareCellTextAt(a, right))
        {
            ++depth;
            if (depth == 0)
                break;
        }
        else if (compareCellTextAt(a, left))
        {
            --depth;
            if (depth == 0)
                break;
        }

        if (auto const prevA = prev(a); prevA != a)
            a = prevA;
        else
            break;
    }
    return a;
}

CellLocation ViCommands::findMatchingPairRight(char left, char right, int initialDepth) const noexcept
{
    auto depth = initialDepth;
    auto b = cursorPosition;

    while (true)
    {
        if (compareCellTextAt(b, left))
        {
            ++depth;
            if (depth == 0)
                break;
        }
        else if (compareCellTextAt(b, right))
        {
            --depth;
            if (depth == 0)
                break;
        }

        if (auto const nextB = next(b); nextB != b)
            b = nextB;
        else
            break;
    }

    return b;
}

CellLocationRange ViCommands::expandMatchingPair(TextObjectScope scope, char left, char right) const noexcept
{
    auto a = findMatchingPairLeft(left, right, left != right ? 1 : -1);
    auto b = findMatchingPairRight(left, right, left != right ? 1 : -1);

    if (scope == TextObjectScope::Inner)
    {
        if (compareCellTextAt(a, left))
            a = next(a);
        if (compareCellTextAt(b, right))
            b = prev(b);
    }

    return { a, b };
}

CellLocationRange ViCommands::translateToCellRange(TextObjectScope scope,
                                                   TextObject textObject) const noexcept
{
    auto const gridTop = -_terminal.currentScreen().historyLineCount().as<LineOffset>();
    auto const gridBottom = _terminal.pageSize().lines.as<LineOffset>() - 1;
    auto const rightMargin = _terminal.pageSize().columns.as<ColumnOffset>() - 1;
    auto a = cursorPosition;
    auto b = cursorPosition;
    switch (textObject)
    {
        case TextObject::AngleBrackets: return expandMatchingPair(scope, '<', '>');
        case TextObject::BackQuotes: return expandMatchingPair(scope, '`', '`');
        case TextObject::CurlyBrackets: return expandMatchingPair(scope, '{', '}');
        case TextObject::DoubleQuotes: return expandMatchingPair(scope, '"', '"');
        case TextObject::Paragraph:
            while (a.line > gridTop && !_terminal.currentScreen().isLineEmpty(a.line - 1))
                --a.line;
            while (b.line < gridBottom && !_terminal.currentScreen().isLineEmpty(b.line))
                ++b.line;
            break;
        case TextObject::RoundBrackets: return expandMatchingPair(scope, '(', ')');
        case TextObject::SingleQuotes: return expandMatchingPair(scope, '\'', '\'');
        case TextObject::SquareBrackets: return expandMatchingPair(scope, '[', ']');
        case TextObject::Word:
            while (a.column.value > 0 && !_terminal.currentScreen().isCellEmpty({ a.line, a.column - 1 }))
                --a.column;
            while (b.column < rightMargin && !_terminal.currentScreen().isCellEmpty({ b.line, b.column }))
                ++b.column;
            break;
    }
    return { a, b };
}

CellLocationRange ViCommands::translateToCellRange(ViMotion motion, unsigned count) const noexcept
{
    switch (motion)
    {
        case ViMotion::FullLine:
            return { cursorPosition - cursorPosition.column,
                     { cursorPosition.line, _terminal.pageSize().columns.as<ColumnOffset>() - 1 } };
        default:
            //.
            return { cursorPosition, translateToCellLocation(motion, count) };
    }
}

CellLocation ViCommands::snapToCell(CellLocation location) const noexcept
{
    while (location.column > ColumnOffset(0) && compareCellTextAt(location, '\0'))
        --location.column;

    return location;
}

CellLocation ViCommands::snapToCellRight(CellLocation location) const noexcept
{
    auto const rightMargin = ColumnOffset::cast_from(_terminal.pageSize().columns - 1);
    while (location.column < rightMargin && compareCellTextAt(location, '\0'))
        ++location.column;
    return location;
}

bool ViCommands::compareCellTextAt(CellLocation position, char codepoint) const noexcept
{
    return _terminal.currentScreen().compareCellTextAt(position, codepoint);
}

CellLocation ViCommands::translateToCellLocation(ViMotion motion, unsigned count) const noexcept
{
    switch (motion)
    {
        case ViMotion::CharLeft: // h
        {
            auto resultPosition = cursorPosition;
            // Jumping left to the next non-empty column (whitespace is not considered empty).
            // This isn't the most efficient implementation, but it's invoked interactively only anyways.
            for (unsigned i = 0; i < count && resultPosition.column > ColumnOffset(0); ++i)
                resultPosition = snapToCell(resultPosition - ColumnOffset(1));
            return resultPosition;
        }
        case ViMotion::CharRight: // l
        {
            auto const cellWidth =
                std::max(uint8_t { 1 }, _terminal.currentScreen().cellWidthAt(cursorPosition));
            auto resultPosition = cursorPosition;
            resultPosition.column += ColumnOffset::cast_from(cellWidth);
            resultPosition.column =
                min(resultPosition.column, ColumnOffset::cast_from(_terminal.pageSize().columns - 1));
            return resultPosition;
        }
        case ViMotion::ScreenColumn: // |
            return snapToCell({ cursorPosition.line,
                                min(ColumnOffset::cast_from(count - 1),
                                    _terminal.pageSize().columns.as<ColumnOffset>() - 1) });
        case ViMotion::FileBegin: // gg
            return snapToCell(
                { LineOffset::cast_from(-_terminal.currentScreen().historyLineCount().as<int>()),
                  ColumnOffset(0) });
        case ViMotion::FileEnd: // G
            return snapToCell({ _terminal.pageSize().lines.as<LineOffset>() - 1, ColumnOffset(0) });
        case ViMotion::PageTop: // <S-H>
            return snapToCell({ boxed_cast<LineOffset>(-_terminal.viewport().scrollOffset())
                                    + *_terminal.viewport().scrollOff(),
                                ColumnOffset(0) });
        case ViMotion::PageBottom: // <S-L>
            return snapToCell({ boxed_cast<LineOffset>(-_terminal.viewport().scrollOffset())
                                    + boxed_cast<LineOffset>(_terminal.pageSize().lines
                                                             - *_terminal.viewport().scrollOff() - 1),
                                ColumnOffset(0) });
        case ViMotion::LineBegin: // 0
            return { cursorPosition.line, ColumnOffset(0) };
        case ViMotion::LineTextBegin: // ^
        {
            auto result = CellLocation { cursorPosition.line, ColumnOffset(0) };
            while (result.column < _terminal.pageSize().columns.as<ColumnOffset>() - 1
                   && _terminal.currentScreen().isCellEmpty(result))
                ++result.column;
            return result;
        }
        case ViMotion::LineDown: // j
            return { min(cursorPosition.line + LineOffset::cast_from(count),
                         _terminal.pageSize().lines.as<LineOffset>() - 1),
                     cursorPosition.column };
        case ViMotion::LineEnd: // $
            return getRightMostNonEmptyCellLocation(_terminal, cursorPosition.line);
        case ViMotion::LineUp: // k
            return { max(cursorPosition.line - LineOffset::cast_from(count),
                         -_terminal.currentScreen().historyLineCount().as<LineOffset>()),
                     cursorPosition.column };
        case ViMotion::LinesCenter: // M
            return { LineOffset::cast_from(_terminal.pageSize().lines / 2 - 1)
                         - boxed_cast<LineOffset>(_terminal.viewport().scrollOffset()),
                     cursorPosition.column };
        case ViMotion::PageDown:
            return { min(cursorPosition.line + LineOffset::cast_from(_terminal.pageSize().lines / 2),
                         _terminal.pageSize().lines.as<LineOffset>() - 1),
                     cursorPosition.column };
        case ViMotion::PageUp:
            return { max(cursorPosition.line - LineOffset::cast_from(_terminal.pageSize().lines / 2),
                         -_terminal.currentScreen().historyLineCount().as<LineOffset>()),
                     cursorPosition.column };
            return cursorPosition
                   - min(cursorPosition.line, LineOffset::cast_from(_terminal.pageSize().lines) / 2);
        case ViMotion::ParagraphBackward: // {
        {
            auto const pageTop = -_terminal.currentScreen().historyLineCount().as<LineOffset>();
            auto prev = CellLocation { cursorPosition.line, ColumnOffset(0) };
            if (prev.line.value > 0)
                prev.line--;
            auto current = prev;
            while (current.line > pageTop
                   && (!_terminal.currentScreen().isLineEmpty(current.line)
                       || _terminal.currentScreen().isLineEmpty(prev.line)))
            {
                prev.line = current.line;
                current.line--;
            }
            return snapToCell(current);
        }
        case ViMotion::ParagraphForward: // }
        {
            auto const pageBottom = _terminal.pageSize().lines.as<LineOffset>() - 1;
            auto prev = CellLocation { cursorPosition.line, ColumnOffset(0) };
            if (prev.line < pageBottom)
                prev.line++;
            auto current = prev;
            while (current.line < pageBottom
                   && (!_terminal.currentScreen().isLineEmpty(current.line)
                       || _terminal.currentScreen().isLineEmpty(prev.line)))
            {
                prev.line = current.line;
                current.line++;
            }
            return snapToCell(current);
        }
        case ViMotion::ParenthesisMatching: // % TODO
            return findMatchingPairFrom(cursorPosition);
        case ViMotion::SearchResultBackward: // N TODO
        case ViMotion::SearchResultForward:  // n TODO
            errorlog()("TODO: Missing implementation. Sorry. That will come. :-)");
            return cursorPosition;
        case ViMotion::WordBackward: { // b
            auto prev = cursorPosition;
            if (prev.column.value > 0)
                prev.column--;
            auto current = prev;

            while (current.column.value > 0
                   && !(!_terminal.wordDelimited(prev) && _terminal.wordDelimited(current)))
            {
                prev.column = current.column;
                current.column--;
            }
            if (current.column.value == 0)
                return current;
            else
                return prev;
        }
        case ViMotion::WordEndForward: { // e
            auto const rightMargin = _terminal.pageSize().columns.as<ColumnOffset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && !(!_terminal.wordDelimited(prev) && _terminal.wordDelimited(current)))
            {
                prev.column = current.column;
                current.column++;
            }
            return prev;
        }
        case ViMotion::WordForward: { // w
            auto const rightMargin = _terminal.pageSize().columns.as<ColumnOffset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && !(_terminal.wordDelimited(prev) ^ _terminal.wordDelimited(current)))
            {
                prev = current;
                current.column++;
            }
            return current;
        }
        case ViMotion::Explicit:  // <special for explicit operations>
        case ViMotion::Selection: // <special for visual modes>
        case ViMotion::FullLine:  // <special for full-line operations>
            return snapToCell(cursorPosition);
    }
    crispy::unreachable();
}

void ViCommands::moveCursor(ViMotion motion, unsigned count)
{
    Require(_terminal.inputHandler().mode() != ViMode::Insert);

    auto const nextPosition = translateToCellLocation(motion, count);
    InputLog()("Move cursor: {} to {}\n", motion, nextPosition);
    moveCursorTo(nextPosition);
}

void ViCommands::moveCursorTo(CellLocation position)
{
    cursorPosition = position;

    _terminal.viewport().makeVisible(cursorPosition.line);

    switch (_terminal.inputHandler().mode())
    {
        case ViMode::Normal:
        case ViMode::Insert: break;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock:
            if (_terminal.selector())
                _terminal.selector()->extend(cursorPosition);
            break;
    }

    _terminal.screenUpdated();
}

} // namespace terminal
