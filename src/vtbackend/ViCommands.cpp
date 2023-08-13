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
#include <vtbackend/primitives.h>

#include <fmt/format.h>

#include <memory>

#include <libunicode/ucd.h>

namespace terminal
{

namespace
{
    constexpr bool isWord(char32_t codepoint) noexcept
    {
        // A word consists of a sequence of letters, digits and underscores, or a
        // sequence of other non-blank characters, separated with white space (spaces,
        // tabs, <EOL>).  This can be changed with the 'iskeyword' option.  An empty line
        // is also considered to be a word.
        return ('a' <= codepoint && codepoint <= 'z') || ('A' <= codepoint && codepoint <= 'Z')
               || ('0' <= codepoint && codepoint <= '9') || codepoint == '_';
    }

    constexpr bool isKeyword(char32_t codepoint) noexcept
    {
        // vim default: (default: @,48-57,_,192-255)
        //
        // For '@' characters above 255 check the "word" character class
        // (any character that is not white space or punctuation).
        //
        // TODO: The punctuation test is highly inefficient. Adapt libunicode to allow O(1) access to these.
        return (codepoint > 255
                && !(unicode::general_category::space_separator(codepoint)
                     || unicode::general_category::initial_punctuation(codepoint)
                     || unicode::general_category::final_punctuation(codepoint)
                     || unicode::general_category::open_punctuation(codepoint)
                     || unicode::general_category::close_punctuation(codepoint)
                     || unicode::general_category::dash_punctuation(codepoint)))
               || (192 <= codepoint && codepoint <= 255);
    }

    enum class word_skip_class
    {
        Word,
        Keyword,
        Whitespace,
        Other
    };

    [[maybe_unused]] std::string_view str(word_skip_class value)
    {
        switch (value)
        {
            case word_skip_class::Word: return "Word";
            case word_skip_class::Keyword: return "Keyword";
            case word_skip_class::Whitespace: return "Whitespace";
            case word_skip_class::Other: return "Other";
        }
        return "Wow";
    }

    constexpr word_skip_class wordSkipClass(char32_t codepoint) noexcept
    {
        if (isWord(codepoint))
            return word_skip_class::Word;
        else if (isKeyword(codepoint))
            return word_skip_class::Keyword;
        else if (codepoint == ' ' || codepoint == '\t' || codepoint == 0)
            return word_skip_class::Whitespace;
        else
            return word_skip_class::Other;
    }

    word_skip_class wordSkipClass(std::string text) noexcept
    {
        auto const s32 = unicode::convert_to<char32_t>(std::string_view(text.data(), text.size()));
        switch (s32.size())
        {
            case 0: return word_skip_class::Whitespace;
            case 1: return wordSkipClass(s32[0]);
            default: return word_skip_class::Other;
        }
    }

    // constexpr bool shouldSkipForUntilWordBeginReverse(WordSkipClass current, WordSkipClass& initial)
    // noexcept
    // {
    //     auto const result = current == initial
    //                         || (current == WordSkipClass::Whitespace && initial !=
    //                         WordSkipClass::Whitespace);
    //     return result;
    // }

    constexpr bool shouldSkipForUntilWordBegin(word_skip_class current, word_skip_class& initial) noexcept
    {
        bool const result =
            current == initial
            || (current == word_skip_class::Whitespace && initial != word_skip_class::Whitespace);

        if (current == word_skip_class::Whitespace && initial != word_skip_class::Whitespace)
            initial = word_skip_class::Whitespace;

        return result;
    }

    cell_location getRightMostNonEmptyCellLocation(Terminal const& terminal, line_offset lineOffset) noexcept
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

    constexpr bool isValidCharMove(std::optional<vi_motion> motion) noexcept
    {
        if (!motion.has_value())
            return false;
        switch (*motion)
        {
            case vi_motion::TillBeforeCharRight:
            case vi_motion::TillAfterCharLeft:
            case vi_motion::ToCharRight:
            case vi_motion::ToCharLeft: return true;
            default: return false;
        }
    }

    constexpr vi_motion invertCharMove(vi_motion motion) noexcept
    {
        switch (motion)
        {
            case vi_motion::TillBeforeCharRight: return vi_motion::TillAfterCharLeft;
            case vi_motion::TillAfterCharLeft: return vi_motion::TillBeforeCharRight;
            case vi_motion::ToCharRight: return vi_motion::ToCharLeft;
            case vi_motion::ToCharLeft: return vi_motion::ToCharRight;
            default: return motion;
        }
    }

} // namespace

using namespace std;

vi_commands::vi_commands(Terminal& theTerminal): _terminal { theTerminal }
{
}

void vi_commands::scrollViewport(scroll_offset delta)
{
    if (delta.value < 0)
        _terminal.get_viewport().scrollDown(boxed_cast<LineCount>(-delta));
    else
        _terminal.get_viewport().scrollUp(boxed_cast<LineCount>(delta));
}

void vi_commands::searchStart()
{
    _terminal.screenUpdated();
}

void vi_commands::searchDone()
{
    _terminal.screenUpdated();
}

void vi_commands::searchCancel()
{
    _terminal.state().searchMode.pattern.clear();
    _terminal.screenUpdated();
}

bool vi_commands::jumpToNextMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        auto startPosition = cursorPosition;
        if (startPosition.column < boxed_cast<column_offset>(_terminal.pageSize().columns))
            startPosition.column++;
        else if (cursorPosition.line < boxed_cast<line_offset>(_terminal.pageSize().lines) - 1)
        {
            startPosition.line++;
            startPosition.column = column_offset(0);
        }

        auto const nextPosition = _terminal.search(startPosition);
        if (!nextPosition)
            return false;

        moveCursorTo(nextPosition.value());
    }
    return true;
}

bool vi_commands::jumpToPreviousMatch(unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        auto startPosition = cursorPosition;
        if (startPosition.column != column_offset(0))
            startPosition.column--;
        else if (cursorPosition.line > -boxed_cast<line_offset>(_terminal.currentScreen().historyLineCount()))
        {
            startPosition.line--;
            startPosition.column = boxed_cast<column_offset>(_terminal.pageSize().columns) - 1;
        }

        auto const nextPosition = _terminal.searchReverse(startPosition);
        if (!nextPosition)
            return false;

        moveCursorTo(nextPosition.value());
    }
    return true;
}

void vi_commands::updateSearchTerm(std::u32string const& text)
{
    if (auto const newLocation = _terminal.searchReverse(text, cursorPosition))
        moveCursorTo(newLocation.value());
}

void vi_commands::modeChanged(vi_mode mode)
{
    auto _ = crispy::finally { [this, mode]() {
        _lastMode = mode;
    } };

    InputLog()("mode changed to {}\n", mode);

    auto const selectFrom = _terminal.selector() ? _terminal.selector()->from() : cursorPosition;

    switch (mode)
    {
        case vi_mode::Insert:
            // Force re-render as viewport & cursor might have changed.
            _terminal.setMode(dec_mode::VisibleCursor, _lastCursorVisible);
            _terminal.setCursorShape(_lastCursorShape);
            _terminal.get_viewport().forceScrollToBottom();
            _terminal.clearSearch();
            _terminal.popStatusDisplay();
            _terminal.screenUpdated();
            break;
        case vi_mode::Normal:
            _lastCursorShape = _terminal.cursorShape();
            _lastCursorVisible = _terminal.isModeEnabled(dec_mode::VisibleCursor);
            _terminal.setMode(dec_mode::VisibleCursor, true);

            if (_lastMode == vi_mode::Insert)
                cursorPosition = _terminal.currentScreen().cursor().position;
            if (_terminal.selectionAvailable())
                _terminal.clearSelection();
            _terminal.pushStatusDisplay(status_display_type::Indicator);
            _terminal.screenUpdated();
            break;
        case vi_mode::Visual:
            _terminal.setSelector(make_unique<LinearSelection>(
                _terminal.selectionHelper(), selectFrom, _terminal.selectionUpdatedHelper()));
            (void) _terminal.selector()->extend(cursorPosition);
            _terminal.pushStatusDisplay(status_display_type::Indicator);
            break;
        case vi_mode::VisualLine:
            _terminal.setSelector(make_unique<FullLineSelection>(
                _terminal.selectionHelper(), selectFrom, _terminal.selectionUpdatedHelper()));
            (void) _terminal.selector()->extend(cursorPosition);
            _terminal.pushStatusDisplay(status_display_type::Indicator);
            _terminal.screenUpdated();
            break;
        case vi_mode::VisualBlock:
            _terminal.setSelector(make_unique<RectangularSelection>(
                _terminal.selectionHelper(), selectFrom, _terminal.selectionUpdatedHelper()));
            (void) _terminal.selector()->extend(cursorPosition);
            _terminal.pushStatusDisplay(status_display_type::Indicator);
            _terminal.screenUpdated();
            break;
    }

    _terminal.inputModeChanged(mode);
}

void vi_commands::reverseSearchCurrentWord()
{
    // auto const oldPos = cursorPosition;
    auto const [wordUnderCursor, range] = _terminal.extractWordUnderCursor(cursorPosition);
    assert(range.contains(cursorPosition));
    cursorPosition = range.first;

    updateSearchTerm(wordUnderCursor);
    jumpToPreviousMatch(1);
}

void vi_commands::toggleLineMark()
{
    auto const currentLineFlags = _terminal.currentScreen().lineFlagsAt(cursorPosition.line);
    _terminal.currentScreen().enableLineFlags(
        cursorPosition.line, line_flags::Marked, !unsigned(currentLineFlags & line_flags::Marked));
}

void vi_commands::searchCurrentWord()
{
    auto const [wordUnderCursor, range] = _terminal.extractWordUnderCursor(cursorPosition);
    assert(range.contains(cursorPosition));
    cursorPosition = range.second;
    updateSearchTerm(wordUnderCursor);
    jumpToNextMatch(1);
}

void vi_commands::executeYank(vi_motion motion, unsigned count)
{
    switch (motion)
    {
        case vi_motion::Selection: {
            assert(_terminal.selector());
            if (_lastMode == vi_mode::VisualBlock)
                _terminal.setHighlightRange(
                    rectangular_highlight { _terminal.selector()->from(), _terminal.selector()->to() });
            else
                _terminal.setHighlightRange(
                    linear_highlight { _terminal.selector()->from(), _terminal.selector()->to() });
            _terminal.copyToClipboard(_terminal.extractSelectionText());
            _terminal.inputHandler().setMode(vi_mode::Normal);
            break;
        }
        default: {
            auto const [from, to] = translateToCellRange(motion, count);
            executeYank(from, to);
        }
        break;
    }
}

void vi_commands::executeYank(cell_location from, cell_location to)
{
    assert(_terminal.inputHandler().mode() == vi_mode::Normal);
    assert(!_terminal.selector());

    // TODO: ideally keep that selection for about N msecs,
    // such that it'll be visually rendered and the user has a feedback of what's
    // being clipboarded.
    // Maybe via a event API to inform that a non-visual selection
    // has happened and that it can now either be instantly destroyed
    // or delayed (N msecs, configurable),
    _terminal.setSelector(
        make_unique<LinearSelection>(_terminal.selectionHelper(), from, _terminal.selectionUpdatedHelper()));
    (void) _terminal.selector()->extend(to);
    auto const text = _terminal.extractSelectionText();
    _terminal.copyToClipboard(text);
    _terminal.clearSelection();
    _terminal.setHighlightRange(linear_highlight { from, to });
    _terminal.inputHandler().setMode(vi_mode::Normal);
    _terminal.screenUpdated();
}

void vi_commands::execute(vi_operator op, vi_motion motion, unsigned count, char32_t lastChar)
{
    InputLog()("{}: Executing: {} {} {}\n", _terminal.inputHandler().mode(), count, op, motion);
    switch (op)
    {
        case vi_operator::MoveCursor:
            //.
            moveCursor(motion, count);
            break;
        case vi_operator::Yank:
            //.
            if (isValidCharMove(motion))
            {
                _lastCharMotion = motion;
                _lastChar = lastChar;
            }
            executeYank(motion, count);
            break;
        case vi_operator::Paste:
            //.
            _terminal.sendPasteFromClipboard(count, false);
            break;
        case vi_operator::PasteStripped:
            //.
            _terminal.sendPasteFromClipboard(count, true);
            break;
        case vi_operator::ReverseSearchCurrentWord: // TODO: Does this even make sense to have?
            break;
    }
    _terminal.screenUpdated();
}

void vi_commands::select(text_object_scope scope, text_object textObject)
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
    (void) _terminal.selector()->extend(to);
    _terminal.screenUpdated();
}

void vi_commands::yank(text_object_scope scope, text_object textObject)
{
    auto const [from, to] = translateToCellRange(scope, textObject);
    cursorPosition = from;
    InputLog()("{}: Executing: yank {} {}\n", _terminal.inputHandler().mode(), scope, textObject);
    executeYank(from, to);
    _terminal.screenUpdated();
}

void vi_commands::yank(vi_motion motion)
{
    auto const [from, to] = translateToCellRange(motion, 1);
    cursorPosition = from;
    InputLog()("{}: Executing: motion-yank {}\n", _terminal.inputHandler().mode(), motion);
    executeYank(from, to);
    _terminal.screenUpdated();
}

void vi_commands::paste(unsigned count, bool stripped)
{
    _terminal.sendPasteFromClipboard(count, stripped);
}

cell_location vi_commands::prev(cell_location location) const noexcept
{
    if (location.column.value > 0)
        return { location.line, location.column - 1 };

    auto const topLineOffset = _terminal.isPrimaryScreen()
                                   ? -boxed_cast<line_offset>(_terminal.primaryScreen().historyLineCount())
                                   : line_offset(0);
    if (location.line > topLineOffset)
    {
        location = getRightMostNonEmptyCellLocation(_terminal, location.line - 1);
        if (location.column + 1 < boxed_cast<column_offset>(_terminal.pageSize().columns))
            ++location.column;
    }

    return location;
}

cell_location vi_commands::next(cell_location location) const noexcept
{
    auto const rightMargin = _terminal.pageSize().columns.as<column_offset>() - 1;
    if (location.column < rightMargin)
    {
        auto const width = max(uint8_t { 1 }, _terminal.currentScreen().cellWidthAt(location));
        return { location.line, location.column + column_offset::cast_from(width) };
    }

    if (location.line < boxed_cast<line_offset>(_terminal.pageSize().lines - 1))
    {
        location.line++;
        location.column = column_offset(0);
    }

    return location;
}

cell_location vi_commands::findMatchingPairFrom(cell_location location) const noexcept
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

cell_location vi_commands::findMatchingPairLeft(char left, char right, int initialDepth) const noexcept
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

cell_location vi_commands::findMatchingPairRight(char left, char right, int initialDepth) const noexcept
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

cell_location_range vi_commands::expandMatchingPair(text_object_scope scope,
                                                    char left,
                                                    char right) const noexcept
{
    auto a = findMatchingPairLeft(left, right, left != right ? 1 : -1);
    auto b = findMatchingPairRight(left, right, left != right ? 1 : -1);

    if (scope == text_object_scope::Inner)
    {
        if (compareCellTextAt(a, left))
            a = next(a);
        if (compareCellTextAt(b, right))
            b = prev(b);
    }

    return { a, b };
}

cell_location_range vi_commands::translateToCellRange(text_object_scope scope,
                                                      text_object textObject) const noexcept
{
    auto const gridTop = -_terminal.currentScreen().historyLineCount().as<line_offset>();
    auto const gridBottom = _terminal.pageSize().lines.as<line_offset>() - 1;
    auto const rightMargin = _terminal.pageSize().columns.as<column_offset>() - 1;
    auto a = cursorPosition;
    auto b = cursorPosition;
    switch (textObject)
    {
        case text_object::AngleBrackets: return expandMatchingPair(scope, '<', '>');
        case text_object::BackQuotes: return expandMatchingPair(scope, '`', '`');
        case text_object::CurlyBrackets: return expandMatchingPair(scope, '{', '}');
        case text_object::DoubleQuotes: return expandMatchingPair(scope, '"', '"');
        case text_object::LineMark:
            // Walk the line upwards until we find a marked line.
            while (
                a.line > gridTop
                && !(unsigned(_terminal.currentScreen().lineFlagsAt(a.line)) & unsigned(line_flags::Marked)))
                --a.line;
            if (scope == text_object_scope::Inner && a != cursorPosition)
                ++a.line;
            // Walk the line downwards until we find a marked line.
            while (
                b.line < gridBottom
                && !(unsigned(_terminal.currentScreen().lineFlagsAt(b.line)) & unsigned(line_flags::Marked)))
                ++b.line;
            if (scope == text_object_scope::Inner && b != cursorPosition)
                --b.line;
            // Span the range from left most column to right most column.
            a.column = column_offset(0);
            b.column = rightMargin;
            break;
        case text_object::Paragraph:
            while (a.line > gridTop && !_terminal.currentScreen().isLineEmpty(a.line - 1))
                --a.line;
            while (b.line < gridBottom && !_terminal.currentScreen().isLineEmpty(b.line))
                ++b.line;
            break;
        case text_object::RoundBrackets: return expandMatchingPair(scope, '(', ')');
        case text_object::SingleQuotes: return expandMatchingPair(scope, '\'', '\'');
        case text_object::SquareBrackets: return expandMatchingPair(scope, '[', ']');
        case text_object::Word: {
            a = findBeginOfWordAt(a, jump_over::No);
            b = findEndOfWordAt(b, jump_over::No);
            break;
        }
        case text_object::BigWord: {
            while (a.column.value > 0 && !_terminal.currentScreen().isCellEmpty(prev(a)))
                a = prev(a);
            while (b.column < rightMargin && !_terminal.currentScreen().isCellEmpty(next(b)))
                b = next(b);
            break;
        }
    }
    return { a, b };
}

cell_location_range vi_commands::translateToCellRange(vi_motion motion, unsigned count) const noexcept
{
    switch (motion)
    {
        case vi_motion::FullLine:
            return { cursorPosition - cursorPosition.column,
                     { cursorPosition.line, _terminal.pageSize().columns.as<column_offset>() - 1 } };
        default:
            //.
            return { cursorPosition, translateToCellLocation(motion, count) };
    }
}

cell_location vi_commands::findBeginOfWordAt(cell_location location, jump_over jumpOver) const noexcept
{
    auto const firstAddressableLocation =
        cell_location { -line_offset::cast_from(_terminal.currentScreen().historyLineCount()),
                        column_offset(0) };

    auto current = location;
    auto leftLocation = prev(current);
    auto leftClass = wordSkipClass(_terminal.currentScreen().cellTextAt(leftLocation));
    auto continuationClass =
        jumpOver == jump_over::Yes ? leftClass : wordSkipClass(_terminal.currentScreen().cellTextAt(current));

    while (current != firstAddressableLocation && leftClass == continuationClass)
    {
        current = leftLocation;
        leftLocation = prev(current);
        leftClass = wordSkipClass(_terminal.currentScreen().cellTextAt(leftLocation));
        if (continuationClass == word_skip_class::Whitespace && leftClass != word_skip_class::Whitespace)
            continuationClass = leftClass;
    }

    return current;
}

cell_location vi_commands::findEndOfWordAt(cell_location location, jump_over jumpOver) const noexcept
{
    auto const rightMargin = _terminal.pageSize().columns.as<column_offset>();
    auto leftOfCurrent = location;
    if (leftOfCurrent.column + 1 < rightMargin && jumpOver == jump_over::Yes)
        leftOfCurrent.column++;
    auto current = leftOfCurrent;
    while (current.column + 1 < rightMargin
           && !(!_terminal.wordDelimited(leftOfCurrent) && _terminal.wordDelimited(current)))
    {
        leftOfCurrent.column = current.column;
        current.column++;
    }
    return leftOfCurrent;
}

cell_location vi_commands::snapToCell(cell_location location) const noexcept
{
    while (location.column > column_offset(0) && compareCellTextAt(location, '\0'))
        --location.column;

    return location;
}

cell_location vi_commands::snapToCellRight(cell_location location) const noexcept
{
    auto const rightMargin = column_offset::cast_from(_terminal.pageSize().columns - 1);
    while (location.column < rightMargin && compareCellTextAt(location, '\0'))
        ++location.column;
    return location;
}

bool vi_commands::compareCellTextAt(cell_location position, char codepoint) const noexcept
{
    return _terminal.currentScreen().compareCellTextAt(position, codepoint);
}

cell_location vi_commands::globalCharUp(cell_location location, char ch, unsigned count) const noexcept
{
    auto const pageTop = -_terminal.currentScreen().historyLineCount().as<line_offset>();
    auto result = cell_location { location.line, column_offset(0) };
    while (count > 0)
    {
        if (location.column == column_offset(0) && result.line > pageTop)
            --result.line;
        while (result.line > pageTop)
        {
            auto const& line = _terminal.currentScreen().lineTextAt(result.line, false, true);
            if (line.size() == 1 && line[0] == ch)
                break;
            --result.line;
        }
        --count;
    }
    return result;
}

cell_location vi_commands::globalCharDown(cell_location location, char ch, unsigned count) const noexcept
{
    auto const pageBottom = _terminal.pageSize().lines.as<line_offset>() - 1;
    auto result = cell_location { location.line, column_offset(0) };
    while (count > 0)
    {
        if (location.column == column_offset(0) && result.line < pageBottom)
            ++result.line;
        while (result.line < pageBottom)
        {
            auto const& line = _terminal.currentScreen().lineTextAt(result.line, false, true);
            if (line.size() == 1 && line[0] == ch)
                break;
            ++result.line;
        }
        --count;
    }
    return result;
}

cell_location vi_commands::translateToCellLocation(vi_motion motion, unsigned count) const noexcept
{
    switch (motion)
    {
        case vi_motion::CharLeft: // h
        {
            auto resultPosition = cursorPosition;
            while (count)
            {
                resultPosition = prev(resultPosition);
                --count;
            }
            return resultPosition;
        }
        case vi_motion::CharRight: // l
        {
            auto resultPosition = cursorPosition;
            while (count)
            {
                resultPosition = next(resultPosition);
                --count;
            }
            return resultPosition;
        }
        case vi_motion::ScreenColumn: // |
            return snapToCell({ cursorPosition.line,
                                min(column_offset::cast_from(count - 1),
                                    _terminal.pageSize().columns.as<column_offset>() - 1) });
        case vi_motion::FileBegin: // gg
            return snapToCell(
                { line_offset::cast_from(-_terminal.currentScreen().historyLineCount().as<int>()),
                  column_offset(0) });
        case vi_motion::FileEnd: // G
            return snapToCell({ _terminal.pageSize().lines.as<line_offset>() - 1, column_offset(0) });
        case vi_motion::PageTop: // <S-H>
            return snapToCell({ boxed_cast<line_offset>(-_terminal.get_viewport().scrollOffset())
                                    + *_terminal.get_viewport().scrollOff(),
                                column_offset(0) });
        case vi_motion::PageBottom: // <S-L>
            return snapToCell({ boxed_cast<line_offset>(-_terminal.get_viewport().scrollOffset())
                                    + boxed_cast<line_offset>(_terminal.pageSize().lines
                                                              - *_terminal.get_viewport().scrollOff() - 1),
                                column_offset(0) });
        case vi_motion::LineBegin: // 0
            return { cursorPosition.line, column_offset(0) };
        case vi_motion::LineTextBegin: // ^
        {
            auto result = cell_location { cursorPosition.line, column_offset(0) };
            while (result.column < _terminal.pageSize().columns.as<column_offset>() - 1
                   && _terminal.currentScreen().isCellEmpty(result))
                ++result.column;
            return result;
        }
        case vi_motion::LineDown: // j
            return { min(cursorPosition.line + line_offset::cast_from(count),
                         _terminal.pageSize().lines.as<line_offset>() - 1),
                     cursorPosition.column };
        case vi_motion::LineEnd: // $
            return getRightMostNonEmptyCellLocation(_terminal, cursorPosition.line);
        case vi_motion::LineUp: // k
            return { max(cursorPosition.line - line_offset::cast_from(count),
                         -_terminal.currentScreen().historyLineCount().as<line_offset>()),
                     cursorPosition.column };
        case vi_motion::LinesCenter: // M
            return { line_offset::cast_from(_terminal.pageSize().lines / 2 - 1)
                         - boxed_cast<line_offset>(_terminal.get_viewport().scrollOffset()),
                     cursorPosition.column };
        case vi_motion::PageDown:
            return { min(cursorPosition.line + line_offset::cast_from(_terminal.pageSize().lines / 2),
                         _terminal.pageSize().lines.as<line_offset>() - 1),
                     cursorPosition.column };
        case vi_motion::PageUp:
            return { max(cursorPosition.line - line_offset::cast_from(_terminal.pageSize().lines / 2),
                         -_terminal.currentScreen().historyLineCount().as<line_offset>()),
                     cursorPosition.column };
            return cursorPosition
                   - min(cursorPosition.line, line_offset::cast_from(_terminal.pageSize().lines) / 2);
        case vi_motion::ParagraphBackward: // {
        {
            auto const pageTop = -_terminal.currentScreen().historyLineCount().as<line_offset>();
            auto prev = cell_location { cursorPosition.line, column_offset(0) };
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
        case vi_motion::GlobalCurlyOpenUp: // [[
            return globalCharUp(cursorPosition, '{', count);
        case vi_motion::GlobalCurlyOpenDown: // ]]
            return globalCharDown(cursorPosition, '{', count);
        case vi_motion::GlobalCurlyCloseUp: // []
            return globalCharUp(cursorPosition, '}', count);
        case vi_motion::GlobalCurlyCloseDown: // ][
            return globalCharDown(cursorPosition, '}', count);
        case vi_motion::LineMarkUp: // [m
        {
            auto const gridTop = -_terminal.currentScreen().historyLineCount().as<line_offset>();
            auto result = cell_location { cursorPosition.line, column_offset(0) };
            while (count > 0)
            {
                if (result.line > gridTop
                    && _terminal.currentScreen().isLineFlagEnabledAt(result.line, line_flags::Marked))
                    --result.line;
                while (result.line > gridTop
                       && !_terminal.currentScreen().isLineFlagEnabledAt(result.line, line_flags::Marked))
                    --result.line;
                --count;
            }
            return result;
        }
        case vi_motion::LineMarkDown: // ]m
        {
            auto const pageBottom = _terminal.pageSize().lines.as<line_offset>() - 1;
            auto result = cell_location { cursorPosition.line, column_offset(0) };
            while (count > 0)
            {
                if (cursorPosition.column == column_offset(0) && result.line < pageBottom)
                    ++result.line;
                while (result.line < pageBottom)
                {
                    if (unsigned(_terminal.currentScreen().lineFlagsAt(result.line))
                        & unsigned(line_flags::Marked))
                        break;
                    ++result.line;
                }
                --count;
            }
            return result;
        }
        case vi_motion::ParagraphForward: // }
        {
            auto const pageBottom = _terminal.pageSize().lines.as<line_offset>() - 1;
            auto prev = cell_location { cursorPosition.line, column_offset(0) };
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
        case vi_motion::ParenthesisMatching: // % TODO
            return findMatchingPairFrom(cursorPosition);
        case vi_motion::SearchResultBackward: // N TODO
        {
            auto startPosition = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
            {
                startPosition = prev(startPosition);
                auto const nextPosition = _terminal.searchReverse(startPosition);
                if (!nextPosition)
                    return cursorPosition;

                startPosition = *nextPosition;
            }
            return startPosition;
        }
        case vi_motion::SearchResultForward: // n
        {
            auto startPosition = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
            {
                startPosition = next(startPosition);
                auto const nextPosition = _terminal.search(startPosition);
                if (!nextPosition)
                    return cursorPosition;
                startPosition = *nextPosition;
            }
            return startPosition;
        }
        case vi_motion::WordBackward: // b
        {
            auto current = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
                current = findBeginOfWordAt(current, jump_over::Yes);

            return current;
        }
        case vi_motion::WordEndForward: // e
        {
            auto current = cursorPosition;
            for (unsigned i = 0; i < count; ++i)
                current = findEndOfWordAt(cursorPosition, jump_over::Yes);
            return current;
        }
        case vi_motion::BigWordForward: // W
        {
            auto const rightMargin = _terminal.pageSize().columns.as<column_offset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && (_terminal.currentScreen().isCellEmpty(current)
                       || !_terminal.currentScreen().isCellEmpty(prev)))
            {
                prev = current;
                current.column++;
            }
            return current;
        }
        case vi_motion::BigWordEndForward: // E
        {
            auto const rightMargin = _terminal.pageSize().columns.as<column_offset>();
            auto prev = cursorPosition;
            if (prev.column + 1 < rightMargin)
                prev.column++;
            auto current = prev;
            while (current.column + 1 < rightMargin
                   && (!_terminal.currentScreen().isCellEmpty(current)
                       || _terminal.currentScreen().isCellEmpty(prev)))
            {
                prev.column = current.column;
                current.column++;
            }
            return prev;
        }
        case vi_motion::BigWordBackward: // B
        {
            auto prev = cursorPosition;
            if (prev.column.value > 0)
                prev.column--;
            auto current = prev;

            while (current.column.value > 0
                   && (!_terminal.currentScreen().isCellEmpty(current)
                       || _terminal.currentScreen().isCellEmpty(prev)))
            {
                prev.column = current.column;
                current.column--;
            }
            if (current.column.value == 0)
                return current;
            else
                return prev;
        }
        case vi_motion::WordForward: // w
        {
            auto const lastAddressableLocation =
                cell_location { line_offset::cast_from(_terminal.pageSize().lines - 1),
                                column_offset::cast_from(_terminal.pageSize().columns - 1) };
            auto result = cursorPosition;
            while (count > 0)
            {
                auto initialClass = wordSkipClass(_terminal.currentScreen().cellTextAt(result));
                result = next(result);
                while (result != lastAddressableLocation
                       && shouldSkipForUntilWordBegin(
                           wordSkipClass(_terminal.currentScreen().cellTextAt(result)), initialClass))
                    result = next(result);
                --count;
            }

            return result;
        }
        case vi_motion::Explicit:  // <special for explicit operations>
        case vi_motion::Selection: // <special for visual modes>
        case vi_motion::FullLine:  // <special for full-line operations>
            return snapToCell(cursorPosition);
        case vi_motion::TillBeforeCharRight: // t {char}
            if (auto const result = toCharRight(count); result)
                return result.value() - column_offset(1);
            else
                return cursorPosition;
        case vi_motion::TillAfterCharLeft: // T {char}
            if (auto const result = toCharLeft(count); result)
                return result.value() + column_offset(1);
            else
                return cursorPosition;
        case vi_motion::ToCharRight: // f {char}
            return toCharRight(count).value_or(cursorPosition);
        case vi_motion::ToCharLeft: // F {char}
            return toCharLeft(count).value_or(cursorPosition);
        case vi_motion::RepeatCharMove:
            if (isValidCharMove(_lastCharMotion))
                return translateToCellLocation(*_lastCharMotion, count);
            return cursorPosition;
        case vi_motion::RepeatCharMoveReverse:
            if (isValidCharMove(_lastCharMotion))
                return translateToCellLocation(invertCharMove(*_lastCharMotion), count);
            return cursorPosition;
    }
    crispy::unreachable();
}

optional<cell_location> vi_commands::toCharRight(cell_location startPosition) const noexcept
{
    auto result = next(startPosition);

    while (true)
    {
        if (result.line != startPosition.line)
            return std::nullopt;
        if (_terminal.currentScreen().compareCellTextAt(result, _lastChar))
            return result;
        result = next(result);
    }
}

optional<cell_location> vi_commands::toCharLeft(cell_location startPosition) const noexcept
{
    auto result = prev(startPosition);

    while (true)
    {
        if (result.line != startPosition.line)
            return std::nullopt;
        if (_terminal.currentScreen().compareCellTextAt(result, _lastChar))
            return result;
        result = prev(result);
    }
}

optional<cell_location> vi_commands::toCharRight(unsigned count) const noexcept
{
    auto result = optional { cursorPosition };
    while (count > 0 && result.has_value())
    {
        result = toCharRight(*result);
        --count;
    }
    return result;
}

optional<cell_location> vi_commands::toCharLeft(unsigned count) const noexcept
{
    auto result = optional { cursorPosition };
    while (count > 0 && result.has_value())
    {
        result = toCharLeft(*result);
        --count;
    }
    return result;
}

void vi_commands::moveCursor(vi_motion motion, unsigned count, char32_t lastChar)
{
    Require(_terminal.inputHandler().mode() != vi_mode::Insert);

    if (isValidCharMove(motion))
    {
        _lastCharMotion = motion;
        _lastChar = lastChar;
    }

    auto const nextPosition = translateToCellLocation(motion, count);
    InputLog()("Move cursor: {} to {}\n", motion, nextPosition);
    moveCursorTo(nextPosition);
}

void vi_commands::moveCursorTo(cell_location position)
{
    cursorPosition = position;

    _terminal.get_viewport().makeVisibleWithinSafeArea(cursorPosition.line);

    switch (_terminal.inputHandler().mode())
    {
        case vi_mode::Normal:
        case vi_mode::Insert: break;
        case vi_mode::Visual:
        case vi_mode::VisualLine:
        case vi_mode::VisualBlock:
            if (_terminal.selector())
                (void) _terminal.selector()->extend(cursorPosition);
            break;
    }

    _terminal.screenUpdated();
}

} // namespace terminal
