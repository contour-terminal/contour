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
#pragma once

#include <terminal/InputHandler.h>
#include <terminal/Selector.h>
#include <terminal/primitives.h>

#include <crispy/logstore.h>

namespace terminal
{

/*
 * ViInput emulates vi very basic in order to support
 * -------------------------
 *
 * - selecting ranges/lines of text
 * - simple and composed movements
 *
 * FSM could look like this:
 * -------------------------
 *
 * Start      := Count? (Operator | Motion)
 * Count      := [1-9][0-9]*
 * ModeSwitch := i      ; insert mode
 *             | v      ; visual mode
 *             | V      ; visual line mode
 *             | <C-V>  ; visual block mode
 * Operator   := y Motion?
 * Motion     := [jkhl] ; move cursor down/up/left/right
 *             | v      ; enter/leave select mode
 *             | V      ; enter/leave line select mode
 *             | Y      ; yank line
 *             | p      ; leave select mode and paste selection/yanked to stdin
 *             | #      ; reverse search for word below cursor
 *             | w      ; move cursor to next word
 *             | b      ; move cursor to prev word
 *             | e      ; move cursor to end of current word
 *             | \^     ; move cursor to line's first non-space character.
 *             | 0      ; move cursor to BOL
 *             | \$     ; move cursor to EOL
 *             | gg     ; move cursor to BOF (begin of file)
 *             | G      ; move cursor to EOF
 *             | n      ; move cursor to next word that is currently being searched
 *
 * Requirement Examples:
 * ---------------------
 *
 *   3{        move cursor 3 blocks up
 *   5j        move cursor 5 lines down
 *   viw       visual select in word
 *   ya"       yank around "
 */

enum class ViMotion
{
    Explicit,             // <special one for explicit operators>
    Selection,            // <special one for v_ modes>
    FullLine,             // <special one for full-line motions>
    CharLeft,             // h
    CharRight,            // l
    ScreenColumn,         // |
    FileBegin,            // gg
    FileEnd,              // G
    LineBegin,            // 0
    LineTextBegin,        // ^
    LineDown,             // j
    LineEnd,              // $
    LineUp,               // k
    PageDown,             // <C-D>
    PageUp,               // <C-U>
    ParagraphBackward,    // {
    ParagraphForward,     // }
    ParenthesisMatching,  // %
    SearchResultBackward, // N
    SearchResultForward,  // n
    WordBackward,         // b
    WordEndForward,       // e
    WordForward,          // w
};

enum class ViOperator
{
    MoveCursor,
    Yank = 'y',
    Paste = 'p',
    ReverseSearchCurrentWord = '#',
};

enum class ViMode
{
    /// Vi-like normal-mode.
    Normal, // <Escape>, <C-[>

    /// Vi-like insert/terminal mode.
    Insert, // i

    /// Vi-like visual select mode.
    Visual, // v

    /// Vi-like visual line-select mode.
    VisualLine, // V

    /// Vi-like visual block-select mode.
    VisualBlock, // <C-V>

    /// Acts exactly like normal mode, except that visual selection active and visible.
    NormalMotionVisual // special one for yanks in Normal mode.
};

enum class TextObject
{
    AngleBrackets = '<',  // i<  a<
    CurlyBrackets = '{',  // i{  a{
    DoubleQuotes = '"',   // i"  a"
    Paragraph = 'p',      // ip  ap
    RoundBrackets = '(',  // i(  a(
    SingleQuotes = '\'',  // i'  a'
    BackQuotes = '`',     // i`  a`
    SquareBrackets = '[', // i[  a[
    Word = 'w',           // iw  aw
};

enum class TextObjectScope
{
    Inner = 'i',
    A = 'a'
};

using CellLocationRange = std::pair<CellLocation, CellLocation>;

/**
 * ViInputHandler provides Vi-input handling.
 */
class ViInputHandler: public InputHandler
{
  public:
    class Executor
    {
      public:
        virtual ~Executor() = default;

        virtual void execute(ViOperator op, ViMotion motion, unsigned count) = 0;
        virtual void moveCursor(ViMotion motion, unsigned count) = 0;
        virtual void select(TextObjectScope scope, TextObject textObject) = 0;
        virtual void yank(TextObjectScope scope, TextObject textObject) = 0;
        virtual void paste(unsigned count) = 0;

        virtual void modeChanged(ViMode mode) = 0;

        // Starts searching for the word under the cursor position in reverse order.
        // This is like pressing # in Vi.
        virtual void reverseSearchCurrentWord() = 0;
    };

    ViInputHandler(Executor& theExecutor, ViMode initialMode):
        viMode { initialMode }, executor { theExecutor }
    {
    }

    bool sendKeyPressEvent(Key key, Modifier modifier) override;
    bool sendCharPressEvent(char32_t ch, Modifier modifier) override;

    void setMode(ViMode mode);
    void toggleMode(ViMode mode);
    [[nodiscard]] ViMode mode() const noexcept { return viMode; }

    [[nodiscard]] CellLocationRange translateToCellRange(TextObjectScope scope,
                                                         TextObject textObject) const noexcept;

  private:
    bool parseCount(char32_t ch, Modifier modifier);
    bool parseModeSwitch(char32_t ch, Modifier modifier);
    bool parseTextObject(char32_t ch, Modifier modifier);
    void handleNormalMode(char32_t ch, Modifier modifier);
    void handleVisualMode(char32_t ch, Modifier modifier);
    bool handleModeSwitches(char32_t ch, Modifier modifier);
    void execute(ViOperator op, ViMotion motion);
    bool executePendingOrMoveCursor(ViMotion motion);
    void yank(TextObjectScope scope, TextObject textObject);
    void select(TextObjectScope scope, TextObject textObject);

    ViMode viMode = ViMode::Normal;
    unsigned count = 0;
    std::optional<ViOperator> pendingOperator = std::nullopt;
    std::optional<TextObjectScope> pendingTextObjectScope = std::nullopt;
    // std::optional<TextObject> pendingTextObject;
    Executor& executor;
};

} // namespace terminal

namespace fmt // {{{
{
template <>
struct formatter<terminal::ViMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::ViMode mode, FormatContext& ctx)
    {
        using terminal::ViMode;
        switch (mode)
        {
            case ViMode::Normal: return format_to(ctx.out(), "Normal");
            case ViMode::Insert: return format_to(ctx.out(), "Insert");
            case ViMode::Visual: return format_to(ctx.out(), "Visual");
            case ViMode::VisualLine: return format_to(ctx.out(), "VisualLine");
            case ViMode::VisualBlock: return format_to(ctx.out(), "VisualBlock");
            case ViMode::NormalMotionVisual: return format_to(ctx.out(), "NormalMotionVisual");
        }
        return format_to(ctx.out(), "({})", static_cast<unsigned>(mode));
    }
};

template <>
struct formatter<terminal::TextObjectScope>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::TextObjectScope scope, FormatContext& ctx)
    {
        using TextObjectScope = terminal::TextObjectScope;
        switch (scope)
        {
            case TextObjectScope::Inner: return format_to(ctx.out(), "inner");
            case TextObjectScope::A: return format_to(ctx.out(), "a");
        }
        return format_to(ctx.out(), "({})", static_cast<unsigned>(scope));
    }
};

template <>
struct formatter<terminal::TextObject>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::TextObject value, FormatContext& ctx)
    {
        using terminal::TextObject;
        switch (value)
        {
            case TextObject::AngleBrackets: return format_to(ctx.out(), "AngleBrackets");
            case TextObject::BackQuotes: return format_to(ctx.out(), "BackQuotes");
            case TextObject::CurlyBrackets: return format_to(ctx.out(), "CurlyBrackets");
            case TextObject::DoubleQuotes: return format_to(ctx.out(), "DoubleQuotes");
            case TextObject::Paragraph: return format_to(ctx.out(), "Paragraph");
            case TextObject::RoundBrackets: return format_to(ctx.out(), "RoundBrackets");
            case TextObject::SingleQuotes: return format_to(ctx.out(), "SingleQuotes");
            case TextObject::SquareBrackets: return format_to(ctx.out(), "SquareBrackets");
            case TextObject::Word: return format_to(ctx.out(), "Word");
        }
        return format_to(ctx.out(), "({})", static_cast<unsigned>(value));
    }
};
template <>
struct formatter<terminal::ViOperator>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::ViOperator op, FormatContext& ctx)
    {
        using terminal::ViOperator;
        switch (op)
        {
            case ViOperator::MoveCursor: return format_to(ctx.out(), "MoveCursor");
            case ViOperator::Yank: return format_to(ctx.out(), "Yank");
            case ViOperator::Paste: return format_to(ctx.out(), "Paste");
            case ViOperator::ReverseSearchCurrentWord:
                return format_to(ctx.out(), "ReverseSearchCurrentWord");
        }
        return format_to(ctx.out(), "({})", static_cast<unsigned>(op));
    }
};

template <>
struct formatter<terminal::ViMotion>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::ViMotion motion, FormatContext& ctx)
    {
        using terminal::ViMotion;
        switch (motion)
        {
            case ViMotion::Explicit: return format_to(ctx.out(), "Explicit");
            case ViMotion::Selection: return format_to(ctx.out(), "Selection");
            case ViMotion::FullLine: return format_to(ctx.out(), "FullLine");
            case ViMotion::CharLeft: return format_to(ctx.out(), "CharLeft");
            case ViMotion::CharRight: return format_to(ctx.out(), "CharRight");
            case ViMotion::ScreenColumn: return format_to(ctx.out(), "ScreenColumn");
            case ViMotion::FileBegin: return format_to(ctx.out(), "FileBegin");
            case ViMotion::FileEnd: return format_to(ctx.out(), "FileEnd");
            case ViMotion::LineBegin: return format_to(ctx.out(), "LineBegin");
            case ViMotion::LineTextBegin: return format_to(ctx.out(), "LineTextBegin");
            case ViMotion::LineDown: return format_to(ctx.out(), "LineDown");
            case ViMotion::LineEnd: return format_to(ctx.out(), "LineEnd");
            case ViMotion::LineUp: return format_to(ctx.out(), "LineUp");
            case ViMotion::PageDown: return format_to(ctx.out(), "PageDown");
            case ViMotion::PageUp: return format_to(ctx.out(), "PageUp");
            case ViMotion::ParagraphBackward: return format_to(ctx.out(), "ParagraphBackward");
            case ViMotion::ParagraphForward: return format_to(ctx.out(), "ParagraphForward");
            case ViMotion::ParenthesisMatching: return format_to(ctx.out(), "ParenthesisMatching");
            case ViMotion::SearchResultBackward: return format_to(ctx.out(), "SearchResultBackward");
            case ViMotion::SearchResultForward: return format_to(ctx.out(), "SearchResultForward");
            case ViMotion::WordBackward: return format_to(ctx.out(), "WordBackward");
            case ViMotion::WordEndForward: return format_to(ctx.out(), "WordEndForward");
            case ViMotion::WordForward: return format_to(ctx.out(), "WordForward");
        }
        return format_to(ctx.out(), "({})", static_cast<unsigned>(motion));
    }
};

} // namespace fmt
// }}}
