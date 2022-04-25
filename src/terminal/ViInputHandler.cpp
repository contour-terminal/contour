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
// motion f{char}
// motion t{char}
// motion %
// motion to jump marks up/down

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
        case 'w': return TextObject::Word;
        case '{': return TextObject::CurlyBrackets;
        default: return nullopt;
    }
}

optional<ViMotion> charToMotion(char ch) noexcept
{
    switch (ch)
    {
        case 'j': return ViMotion::LineDown;
        case 'k': return ViMotion::LineUp;
        case 'h': return ViMotion::CharLeft;
        case 'l': return ViMotion::CharRight;
        case '0': return ViMotion::LineBegin;
        case '$': return ViMotion::LineEnd;
        case 'g': return ViMotion::FileBegin;
        case 'G': return ViMotion::FileEnd;
        case 'b': return ViMotion::WordBackward;
        case 'e': return ViMotion::WordEndForward;
        case 'w': return ViMotion::WordForward;
        case 'N': return ViMotion::SearchResultBackward;
        case 'n': return ViMotion::SearchResultForward;
        case '|': return ViMotion::ScreenColumn;
        case '{': return ViMotion::ParagraphBackward;
        case '}': return ViMotion::ParagraphForward;
        case '%': return ViMotion::ParenthesisMatching;
        default: return nullopt;
    }
}

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
    crispy::ignore_unused(key, modifier);
    return false;
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

bool ViInputHandler::handleVisualMode(char32_t ch, Modifier modifier)
{
    Require(viMode == ViMode::Visual || viMode == ViMode::VisualLine || viMode == ViMode::VisualBlock);

    if (parseCount(ch, modifier))
        return true;

    if (pendingTextObjectScope)
    {
        if (optional<TextObject> const textObject = charToTextObject(ch))
        {
            select(*pendingTextObjectScope, *textObject);
            return true;
        }
    }

    if (auto const motion = charToMotion(static_cast<char>(ch)))
    {
        execute(pendingOperator.value_or(ViOperator::MoveCursor), motion.value());
        return true;
    }

    if (modifier == Modifier::Control && ch == 'D')
    {
        execute(pendingOperator.value_or(ViOperator::MoveCursor), ViMotion::PageDown);
        return true;
    }

    if (modifier == Modifier::Control && ch == 'U')
    {
        execute(pendingOperator.value_or(ViOperator::MoveCursor), ViMotion::PageUp);
        return true;
    }

    if (modifier == Modifier::Control && ch == 'V')
    {
        setMode(ViMode::VisualBlock);
        return true;
    }

    switch (ch)
    {
        case 27: setMode(ViMode::Normal); return true; // Escape key.
        case '#': executor.reverseSearchCurrentWord(); return true;
        case 'V': setMode(viMode != ViMode::VisualLine ? ViMode::VisualLine : ViMode::Normal); return true;
        case 'Y': execute(ViOperator::Yank, ViMotion::FullLine); return true;
        case 'a': pendingTextObjectScope = TextObjectScope::A; return true;
        case 'i': pendingTextObjectScope = TextObjectScope::Inner; return true;
        case 'v': setMode(viMode != ViMode::Visual ? ViMode::Visual : ViMode::Normal); return true;
        case 'y': execute(ViOperator::Yank, ViMotion::Selection); return true;
        default: return true;
    }
}

bool ViInputHandler::parseTextObject(char32_t ch, Modifier modifier)
{
    if (modifier.any())
        return false;

    if (!(pendingOperator && *pendingOperator == ViOperator::Yank))
        return false;

    switch (ch)
    {
        case 'i': pendingTextObjectScope = TextObjectScope::Inner; return true;
        case 'a': pendingTextObjectScope = TextObjectScope::A; return true;
        default: break;
    }

    if (!pendingTextObjectScope)
        return false;

    if (optional<TextObject> const textObject = charToTextObject(ch))
    {
        yank(*pendingTextObjectScope, *textObject);
        return true;
    }

    return false;
}

bool ViInputHandler::handleNormalMode(char32_t ch, Modifier modifier)
{
    Require(viMode == ViMode::Normal);

    if (parseCount(ch, modifier))
        return true;

    if (parseTextObject(ch, modifier))
        return true;

    if (auto const motion = charToMotion(static_cast<char>(ch)))
    {
        execute(pendingOperator.value_or(ViOperator::MoveCursor), motion.value());
        return true;
    }

    if (modifier == Modifier::Control && ch == 'D')
    {
        execute(pendingOperator.value_or(ViOperator::MoveCursor), ViMotion::PageDown);
        return true;
    }

    if (modifier == Modifier::Control && ch == 'U')
    {
        execute(pendingOperator.value_or(ViOperator::MoveCursor), ViMotion::PageUp);
        return true;
    }

    if (modifier == Modifier::Control && ch == 'V')
    {
        setMode(ViMode::VisualBlock);
        return true;
    }

    switch (ch)
    {
        case '#': executor.reverseSearchCurrentWord(); return true;
        case 'V': setMode(ViMode::VisualLine); return true;
        case 'i': setMode(ViMode::Insert); return true;
        case 'v': setMode(ViMode::Visual); return true;
        case 'p': execute(ViOperator::Paste, ViMotion::Explicit); return true;
        case 'y':
            if (!pendingOperator.has_value())
                pendingOperator = ViOperator::Yank;
            else if (pendingOperator == ViOperator::Yank)
                execute(ViOperator::Yank, ViMotion::FullLine);
            else
                pendingOperator.reset(); // is this good?
            return true;
    }

    return false;
}

} // namespace terminal
