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
#include <terminal/ViInputHandler.h>
#include <terminal/logging.h>

#include <crispy/assert.h>
#include <crispy/utils.h>

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

        constexpr operator uint32_t() const noexcept
        {
            return uint32_t(ch << 5) | uint32_t(modifier.value() & 0b1'1111);
        }
    };

    constexpr InputMatch operator"" _key(char ch)
    {
        return InputMatch { Modifier::None, static_cast<char32_t>(ch) };
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
            case '<': return TextObject::AngleBrackets;
            case '[': return TextObject::SquareBrackets;
            case '\'': return TextObject::SingleQuotes;
            case '`': return TextObject::BackQuotes;
            case 'p': return TextObject::Paragraph;
            case 'w': return TextObject::Word;
            case '{': return TextObject::CurlyBrackets;
            default: return nullopt;
        }
    }

} // namespace

void ViInputHandler::setMode(ViMode theMode)
{
    if (viMode == theMode)
        return;

    viMode = theMode;
    count = 0;
    pendingOperator = nullopt;
    pendingTextObjectScope = nullopt;

    executor.modeChanged(theMode);
}

bool ViInputHandler::sendKeyPressEvent(Key key, Modifier modifier)
{
    // clang-format off
    switch (viMode)
    {
        case ViMode::Insert:
            return false;
        case ViMode::NormalMotionVisual:
            setMode(ViMode::Normal);
            [[fallthrough]];
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

bool ViInputHandler::sendCharPressEvent(char32_t ch, Modifier modifier)
{
    // clang-format off
    switch (viMode)
    {
        case ViMode::Insert:
            return false;
        case ViMode::NormalMotionVisual:
            setMode(ViMode::Normal);
            [[fallthrough]];
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
            if (!count)
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
            count = count * 10 + (ch - '0');
            return true;
    }
    return false;
}

void ViInputHandler::yank(TextObjectScope scope, TextObject textObject)
{
    executor.yank(scope, textObject);

    count = 0;
    pendingOperator.reset();
    pendingTextObjectScope.reset();
}

void ViInputHandler::select(TextObjectScope scope, TextObject textObject)
{
    executor.select(scope, textObject);

    count = 0;
    pendingOperator.reset();
    pendingTextObjectScope.reset();
}

void ViInputHandler::execute(ViOperator op, ViMotion motion)
{
    executor.execute(op, motion, count ? count : 1);

    count = 0;
    pendingOperator.reset();
    pendingTextObjectScope.reset();
}

void ViInputHandler::handleVisualMode(char32_t ch, Modifier modifier)
{
    Require(viMode == ViMode::Visual || viMode == ViMode::VisualLine || viMode == ViMode::VisualBlock);

    if (parseModeSwitch(ch, modifier))
        return;

    if (parseCount(ch, modifier))
        return;

    if (pendingTextObjectScope)
    {
        if (optional<TextObject> const textObject = charToTextObject(ch))
        {
            select(*pendingTextObjectScope, *textObject);
            return;
        }
    }

    switch (InputMatch { modifier, ch })
    {
        case '\033'_key: setMode(ViMode::Normal); return; // Escape key.
        case Modifier::Control | 'V': toggleMode(ViMode::VisualBlock); return;
        case Modifier::Shift | 'V': toggleMode(ViMode::VisualLine); return;
        case 'v'_key: toggleMode(ViMode::Visual); return;
        case '#'_key: executor.reverseSearchCurrentWord(); return;
        case 'Y'_key: execute(ViOperator::Yank, ViMotion::FullLine); return;
        case 'a'_key: pendingTextObjectScope = TextObjectScope::A; return;
        case 'i'_key: pendingTextObjectScope = TextObjectScope::Inner; return;
        case 'y'_key: execute(ViOperator::Yank, ViMotion::Selection); return;
        default: break;
    }

    if (parseTextObject(ch, modifier))
        return;
}

bool ViInputHandler::executePendingOrMoveCursor(ViMotion motion)
{
    switch (pendingOperator.value_or(ViOperator::MoveCursor))
    {
        case ViOperator::MoveCursor: executor.moveCursor(motion, count ? count : 1); break;
        case ViOperator::Yank:
            // XXX executor.yank(pendingTextObjectScope.value(), pending)
            logstore::ErrorLog()("Yank: Implementation coming: {}", motion);
            break;
        case ViOperator::Paste: executor.paste(count ? count : 1); break;
        case ViOperator::ReverseSearchCurrentWord: executor.reverseSearchCurrentWord(); break;
    }

    count = 0;
    pendingOperator.reset();
    pendingTextObjectScope.reset();

    return true;
}

bool ViInputHandler::parseTextObject(char32_t ch, Modifier modifier)
{
    Require(viMode != ViMode::Insert);

    if (viMode != ViMode::Normal || pendingOperator)
    {
        switch (InputMatch { modifier.without(Modifier::Shift), ch })
        {
            case 'i'_key: pendingTextObjectScope = TextObjectScope::Inner; return true;
            case 'a'_key: pendingTextObjectScope = TextObjectScope::A; return true;
        }
    }

    switch (InputMatch { modifier.without(Modifier::Shift), ch })
    {
        case 'D' | Modifier::Control: return executePendingOrMoveCursor(ViMotion::PageDown);
        case 'U' | Modifier::Control: return executePendingOrMoveCursor(ViMotion::PageUp);
        case '$'_key: return executePendingOrMoveCursor(ViMotion::LineEnd);
        case '%'_key: return executePendingOrMoveCursor(ViMotion::ParenthesisMatching);
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
        switch (viMode)
        {
            case ViMode::Insert:
                break;
            case ViMode::NormalMotionVisual:
            case ViMode::Normal:
                if (pendingTextObjectScope && pendingOperator && *pendingOperator == ViOperator::Yank)
                    yank(*pendingTextObjectScope, *textObject);
                break;
            case ViMode::Visual:
            case ViMode::VisualLine:
            case ViMode::VisualBlock:
                if (pendingTextObjectScope)
                    select(*pendingTextObjectScope, *textObject);
                break;
        }
        // clang-format off
        return true;
    }

    return false;
}

void ViInputHandler::toggleMode(ViMode newMode)
{
    setMode(newMode != viMode ? newMode : ViMode::Normal);
}

bool ViInputHandler::parseModeSwitch(char32_t ch, Modifier modifier)
{
    Require(viMode != ViMode::Insert);

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
            if (!pendingOperator && (viMode == ViMode::Normal || viMode == ViMode::NormalMotionVisual))
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
    Require(viMode == ViMode::Normal);

    if (parseModeSwitch(ch, modifier))
        return;

    if (parseCount(ch, modifier))
        return;

    switch (InputMatch { modifier, ch })
    {
        case 'v'_key: toggleMode(ViMode::Visual); return;
        case '#'_key: executor.reverseSearchCurrentWord(); return;
        case 'p'_key: executor.paste(count ? count : 1); return;
        case 'y'_key:
            if (!pendingOperator.has_value())
                pendingOperator = ViOperator::Yank;
            else if (pendingOperator == ViOperator::Yank)
                execute(ViOperator::Yank, ViMotion::FullLine);
            else
                pendingOperator.reset(); // is this good?
            return;
    }

    if (parseTextObject(ch, modifier))
        return;
}

} // namespace terminal
