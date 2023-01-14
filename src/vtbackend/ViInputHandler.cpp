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
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/logging.h>

#include <crispy/assert.h>
#include <crispy/utils.h>

#include <unicode/convert.h>

using std::nullopt;
using std::optional;

namespace terminal
{

// Possible future improvements (aka. nice TODO):
//
// [ ] motion f{char}
// [ ] motion t{char}
// [ ] motion %
// [ ] motion to jump marks up/down
// [ ] add timer to disable selection (needs timer API inside of libterminal)
// [ ] show cursor if it was hidden and restore it when going back to insert mode
// [ ] remember initial cursor shae and restore it when going back to insert mode

namespace
{
    struct InputMatch
    {
        // ViMode mode; // TODO: ideally we also would like to match on input Mode
        Modifier modifier;
        char32_t ch;

        [[nodiscard]] constexpr uint32_t code() const noexcept
        {
            return uint32_t(ch << 5) | uint32_t(modifier.value() & 0b1'1111);
        }

        constexpr operator uint32_t() const noexcept
        {
            return uint32_t(ch << 5) | uint32_t(modifier.value() & 0b1'1111);
        }
    };

    constexpr InputMatch operator"" _key(char ch)
    {
        return InputMatch { Modifier::None, static_cast<char32_t>(ch) };
    }

    constexpr InputMatch operator|(Modifier::Key modifier, char ch) noexcept
    {
        return InputMatch { Modifier { modifier }, (char32_t) ch };
    }

    constexpr InputMatch operator|(char ch, Modifier::Key modifier) noexcept
    {
        return InputMatch { Modifier { modifier }, (char32_t) ch };
    }

    optional<TextObject> charToTextObject(char32_t ch) noexcept
    {
        switch (ch)
        {
            case '"': return TextObject::DoubleQuotes;
            case '(': return TextObject::RoundBrackets;
            case ')': return TextObject::RoundBrackets;
            case '<': return TextObject::AngleBrackets;
            case '>': return TextObject::AngleBrackets;
            case '[': return TextObject::SquareBrackets;
            case '\'': return TextObject::SingleQuotes;
            case ']': return TextObject::SquareBrackets;
            case '`': return TextObject::BackQuotes;
            case 'p': return TextObject::Paragraph;
            case 'w': return TextObject::Word;
            case '{': return TextObject::CurlyBrackets;
            case '}': return TextObject::CurlyBrackets;
            default: return nullopt;
        }
    }

} // namespace

void ViInputHandler::setMode(ViMode theMode)
{
    if (_viMode == theMode)
        return;

    _viMode = theMode;
    _count = 0;
    _pendingOperator = nullopt;
    _pendingTextObjectScope = nullopt;

    _executor.modeChanged(theMode);
}

bool ViInputHandler::sendKeyPressEvent(Key key, Modifier modifier)
{
    if (_searchEditMode != SearchEditMode::Disabled)
    {
        // Do we want to do anything in here?
        // TODO: support cursor movements.
        errorlog()("ViInputHandler: Ignoring key input {}+{}.", modifier, key);
        return true;
    }

    // clang-format off
    switch (_viMode)
    {
        case ViMode::Insert:
            return false;
        case ViMode::Normal:
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock:
            break;
    }
    // clang-format on

    if (modifier.any())
        return true;

    switch (key)
    {
        case Key::DownArrow: return executePendingOrMoveCursor(ViMotion::LineDown);
        case Key::LeftArrow: return executePendingOrMoveCursor(ViMotion::CharLeft);
        case Key::RightArrow: return executePendingOrMoveCursor(ViMotion::CharRight);
        case Key::UpArrow: return executePendingOrMoveCursor(ViMotion::LineUp);
        case Key::Insert: setMode(ViMode::Insert); return true;
        case Key::Home: return executePendingOrMoveCursor(ViMotion::FileBegin);
        case Key::End: return executePendingOrMoveCursor(ViMotion::FileEnd);
        case Key::PageUp: return executePendingOrMoveCursor(ViMotion::PageUp);
        case Key::PageDown: return executePendingOrMoveCursor(ViMotion::PageDown);
        default: break;
    }
    return true;
}

void ViInputHandler::startSearchExternally()
{
    _searchTerm.clear();
    _executor.searchStart();

    if (_viMode != ViMode::Insert)
        _searchEditMode = SearchEditMode::Enabled;
    else
    {
        _searchEditMode = SearchEditMode::ExternallyEnabled;
        setMode(ViMode::Normal);
        // ^^^ So that we can see the statusline (which contains the search edit field),
        // AND it's weird to be in insert mode while typing in the search term anyways.
    }
}

bool ViInputHandler::handleSearchEditor(char32_t ch, Modifier modifier)
{
    assert(_searchEditMode != SearchEditMode::Disabled);

    switch (InputMatch { modifier, ch })
    {
        case '\x1B'_key:
            _searchTerm.clear();
            if (_searchEditMode == SearchEditMode::ExternallyEnabled)
                setMode(ViMode::Insert);
            _searchEditMode = SearchEditMode::Disabled;
            _executor.searchCancel();
            break;
        case '\x0D'_key:
            if (_searchEditMode == SearchEditMode::ExternallyEnabled)
                setMode(ViMode::Insert);
            _searchEditMode = SearchEditMode::Disabled;
            _executor.searchDone();
            break;
        case '\x08'_key:
        case '\x7F'_key:
            if (_searchTerm.size() > 0)
                _searchTerm.resize(_searchTerm.size() - 1);
            _executor.updateSearchTerm(_searchTerm);
            break;
        case Modifier::Control | 'L':
        case Modifier::Control | 'U':
            _searchTerm.clear();
            _executor.updateSearchTerm(_searchTerm);
            break;
        case Modifier::Control | 'A': // TODO: move cursor to BOL
        case Modifier::Control | 'E': // TODO: move cursor to EOL
        default:
            if (ch >= 0x20 && modifier.without(Modifier::Shift).none())
            {
                _searchTerm += ch;
                _executor.updateSearchTerm(_searchTerm);
            }
            else
                errorlog()("ViInputHandler: Receiving control code {}+0x{:02X} in search mode. Ignoring.",
                           modifier,
                           (unsigned) ch);
    }

    return true;
}

bool ViInputHandler::sendCharPressEvent(char32_t ch, Modifier modifier)
{
    if (_searchEditMode != SearchEditMode::Disabled)
        return handleSearchEditor(ch, modifier);

    // clang-format off
    switch (_viMode)
    {
        case ViMode::Insert:
            return false;
        case ViMode::Normal:
            handleNormalMode(ch, modifier);
            return true;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock:
            handleVisualMode(ch, modifier);
            return true;
    }
    // clang-format on

    crispy::unreachable();
}

bool ViInputHandler::parseCount(char32_t ch, Modifier modifier)
{
    if (!modifier.none())
        return false;

    switch (ch)
    {
        case '0':
            if (!_count)
                break;
            [[fallthrough]];
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            //.
            _count = _count * 10 + (ch - '0');
            return true;
    }
    return false;
}

void ViInputHandler::yank(TextObjectScope scope, TextObject textObject)
{
    _executor.yank(scope, textObject);

    _count = 0;
    _pendingOperator.reset();
    _pendingTextObjectScope.reset();
}

void ViInputHandler::select(TextObjectScope scope, TextObject textObject)
{
    _executor.select(scope, textObject);

    _count = 0;
    _pendingOperator.reset();
    _pendingTextObjectScope.reset();
}

void ViInputHandler::execute(ViOperator op, ViMotion motion)
{
    _executor.execute(op, motion, _count ? _count : 1);

    _count = 0;
    _pendingOperator.reset();
    _pendingTextObjectScope.reset();
}

void ViInputHandler::handleVisualMode(char32_t ch, Modifier modifier)
{
    Require(_viMode == ViMode::Visual || _viMode == ViMode::VisualLine || _viMode == ViMode::VisualBlock);

    if (parseModeSwitch(ch, modifier))
        return;

    if (parseCount(ch, modifier))
        return;

    if (_pendingTextObjectScope)
    {
        if (optional<TextObject> const textObject = charToTextObject(ch))
        {
            select(*_pendingTextObjectScope, *textObject);
            return;
        }
    }

    switch (InputMatch { modifier.without(Modifier::Shift), ch })
    {
        case '/'_key: startSearch(); return;
        case '\033'_key: setMode(ViMode::Normal); return; // Escape key.
        case Modifier::Control | 'V': toggleMode(ViMode::VisualBlock); return;
        case 'V'_key: toggleMode(ViMode::VisualLine); return;
        case 'v'_key: toggleMode(ViMode::Visual); return;
        case '#'_key: _executor.reverseSearchCurrentWord(); return;
        case '*'_key: _executor.searchCurrentWord(); return;
        case 'Y'_key: execute(ViOperator::Yank, ViMotion::FullLine); return;
        case 'a'_key: _pendingTextObjectScope = TextObjectScope::A; return;
        case 'i'_key: _pendingTextObjectScope = TextObjectScope::Inner; return;
        case 'y'_key: execute(ViOperator::Yank, ViMotion::Selection); return;
        case 'n'_key: _executor.jumpToNextMatch(_count ? _count : 1); return;
        case 'N' | Modifier::Shift: _executor.jumpToPreviousMatch(_count ? _count : 1); return;
        default: break;
    }

    if (parseTextObject(ch, modifier))
        return;
}

void ViInputHandler::startSearch()
{
    _searchEditMode = SearchEditMode::Enabled;
    _searchTerm.clear();
    _executor.searchStart();
}

void ViInputHandler::scrollViewport(ScrollOffset delta)
{
    _executor.scrollViewport(delta);
}

bool ViInputHandler::executePendingOrMoveCursor(ViMotion motion)
{
    // Require(!_pendingTextObjectScope);
    switch (_pendingOperator.value_or(ViOperator::MoveCursor))
    {
        case ViOperator::MoveCursor: _executor.moveCursor(motion, _count ? _count : 1); break;
        case ViOperator::Yank: _executor.yank(motion); break;
        case ViOperator::Paste: _executor.paste(_count ? _count : 1, false); break;
        case ViOperator::PasteStripped: _executor.paste(_count ? _count : 1, true); break;
        case ViOperator::ReverseSearchCurrentWord: _executor.reverseSearchCurrentWord(); break;
    }

    _count = 0;
    _pendingOperator.reset();
    _pendingTextObjectScope.reset();

    return true;
}

bool ViInputHandler::parseTextObject(char32_t ch, Modifier modifier)
{
    Require(_viMode != ViMode::Insert);

    if (_viMode != ViMode::Normal || _pendingOperator)
    {
        switch (InputMatch { modifier.without(Modifier::Shift), ch })
        {
            case 'i'_key: _pendingTextObjectScope = TextObjectScope::Inner; return true;
            case 'a'_key: _pendingTextObjectScope = TextObjectScope::A; return true;
        }
    }

    if (_pendingTextObjectScope && _pendingOperator)
    {
        if (optional<TextObject> const textObject = charToTextObject(ch))
        {
            switch (*_pendingOperator)
            {
                case ViOperator::Yank: yank(*_pendingTextObjectScope, *textObject); break;
                default:
                    logstore::ErrorLog()(
                        "ViInputHandler: trying to operate on text object with unsupported operator {}.",
                        _pendingOperator.value());
                    break;
            }
            return true;
        }
    }

    switch (InputMatch { modifier.without(Modifier::Shift), ch })
    {
        case 'D' | Modifier::Control: return executePendingOrMoveCursor(ViMotion::PageDown);
        case 'U' | Modifier::Control: return executePendingOrMoveCursor(ViMotion::PageUp);
        case '$'_key: return executePendingOrMoveCursor(ViMotion::LineEnd);
        case '%'_key: return executePendingOrMoveCursor(ViMotion::ParenthesisMatching);
        case 'M'_key: return executePendingOrMoveCursor(ViMotion::LinesCenter);
        case '0'_key: return executePendingOrMoveCursor(ViMotion::LineBegin);
        case '^'_key: return executePendingOrMoveCursor(ViMotion::LineTextBegin);
        case 'G'_key: return executePendingOrMoveCursor(ViMotion::FileEnd);
        case 'N'_key: return executePendingOrMoveCursor(ViMotion::SearchResultBackward);
        case 'b'_key: return executePendingOrMoveCursor(ViMotion::WordBackward);
        case 'e'_key: return executePendingOrMoveCursor(ViMotion::WordEndForward);
        case 'g'_key: return executePendingOrMoveCursor(ViMotion::FileBegin);
        case 'h'_key: return executePendingOrMoveCursor(ViMotion::CharLeft);
        case 'j'_key: return executePendingOrMoveCursor(ViMotion::LineDown);
        case 'k'_key: return executePendingOrMoveCursor(ViMotion::LineUp);
        case 'J'_key: scrollViewport(ScrollOffset(-1)); return executePendingOrMoveCursor(ViMotion::LineDown);
        case 'K'_key: scrollViewport(ScrollOffset(+1)); return executePendingOrMoveCursor(ViMotion::LineUp);
        case 'H'_key: return executePendingOrMoveCursor(ViMotion::PageTop);
        case 'L'_key: return executePendingOrMoveCursor(ViMotion::PageBottom);
        case 'l'_key: return executePendingOrMoveCursor(ViMotion::CharRight);
        case 'n'_key: return executePendingOrMoveCursor(ViMotion::SearchResultForward);
        case 'w'_key: return executePendingOrMoveCursor(ViMotion::WordForward);
        case '{'_key: return executePendingOrMoveCursor(ViMotion::ParagraphBackward);
        case '|'_key: return executePendingOrMoveCursor(ViMotion::ScreenColumn);
        case '}'_key: return executePendingOrMoveCursor(ViMotion::ParagraphForward);
    }

    if (modifier.any())
        return false;

    if (optional<TextObject> const textObject = charToTextObject(ch))
    {
        // clang-format off
        switch (_viMode)
        {
            case ViMode::Insert:
                break;
            case ViMode::Normal:
                if (_pendingTextObjectScope && _pendingOperator && *_pendingOperator == ViOperator::Yank)
                    yank(*_pendingTextObjectScope, *textObject);
                break;
            case ViMode::Visual:
            case ViMode::VisualLine:
            case ViMode::VisualBlock:
                if (_pendingTextObjectScope)
                    select(*_pendingTextObjectScope, *textObject);
                break;
        }
        // clang-format off
        return true;
    }

    return false;
}

void ViInputHandler::toggleMode(ViMode newMode)
{
    setMode(newMode != _viMode ? newMode : ViMode::Normal);
}

bool ViInputHandler::parseModeSwitch(char32_t ch, Modifier modifier)
{
    Require(_viMode != ViMode::Insert);

    switch (InputMatch { modifier, ch })
    {
        case 'V' | Modifier::Control:
            toggleMode(ViMode::VisualBlock);
            return true;
        case 'V' | Modifier::Shift:
            toggleMode(ViMode::VisualLine);
            return true;
        case 'a'_key:
        case 'i'_key:
            if (!_pendingOperator && _viMode == ViMode::Normal )
            {
                toggleMode(ViMode::Insert);
                return true;
            }
            break;
        case 'v'_key:
            toggleMode(ViMode::Visual);
            return true;
    }
    return false;
}

void ViInputHandler::handleNormalMode(char32_t ch, Modifier modifier)
{
    Require(_viMode == ViMode::Normal);

    if (parseModeSwitch(ch, modifier))
        return;

    if (parseCount(ch, modifier))
        return;

    switch (InputMatch { modifier.without(Modifier::Shift), ch })
    {
        case '/'_key: startSearch(); return;
        case 'v'_key: toggleMode(ViMode::Visual); return;
        case '#'_key: _executor.reverseSearchCurrentWord(); return;
        case '*'_key: _executor.searchCurrentWord(); return;
        case 'p'_key: _executor.paste(_count ? _count : 1, false); return;
        case 'P'_key: _executor.paste(_count ? _count : 1, true); return;
        case 'n'_key: _executor.jumpToNextMatch(_count ? _count : 1); return;
        case 'N'_key: _executor.jumpToPreviousMatch(_count ? _count : 1); return;
        case 'y'_key:
            if (!_pendingOperator.has_value())
                _pendingOperator = ViOperator::Yank;
            else if (_pendingOperator == ViOperator::Yank)
                execute(ViOperator::Yank, ViMotion::FullLine);
            else
                _pendingOperator.reset(); // is this good?
            return;
    }

    if (parseTextObject(ch, modifier))
        return;
}

} // namespace terminal
