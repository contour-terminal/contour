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

#include <vtbackend/InputHandler.h>
#include <vtbackend/Selector.h>
#include <vtbackend/primitives.h>

#include <crispy/TrieMap.h>
#include <crispy/assert.h>
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
    Explicit,              // <special one for explicit operators>
    Selection,             // <special one for v_ modes>
    FullLine,              // <special one for full-line motions>
    CharLeft,              // h
    CharRight,             // l
    ScreenColumn,          // |
    FileBegin,             // gg
    FileEnd,               // G
    LineBegin,             // 0
    LineTextBegin,         // ^
    LineDown,              // j
    LineEnd,               // $
    LineUp,                // k
    LinesCenter,           // M
    PageDown,              // <C-D>
    PageUp,                // <C-U>
    PageTop,               // <S-H> (inspired by tmux)
    PageBottom,            // <S-L> (inspired by tmux)
    ParagraphBackward,     // {
    ParagraphForward,      // }
    GlobalCurlyCloseUp,    // []
    GlobalCurlyCloseDown,  // ][
    GlobalCurlyOpenUp,     // [[
    GlobalCurlyOpenDown,   // ]]
    LineMarkUp,            // [m
    LineMarkDown,          // ]m
    ParenthesisMatching,   // %
    SearchResultBackward,  // N
    SearchResultForward,   // n
    WordBackward,          // b
    WordEndForward,        // e
    WordForward,           // w
    BigWordBackward,       // B
    BigWordEndForward,     // E
    BigWordForward,        // W
    TillBeforeCharRight,   // t {char}
    TillAfterCharLeft,     // T {char}
    ToCharRight,           // f {char}
    ToCharLeft,            // F {char}
    RepeatCharMove,        // ;
    RepeatCharMoveReverse, // ,
};

enum class ViOperator
{
    MoveCursor,
    Yank = 'y',
    Paste = 'p',
    PasteStripped = 'P',
    ReverseSearchCurrentWord = '#',
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
    BigWord = 'W',        // iW  aW
};

enum class TextObjectScope
{
    Inner = 'i',
    A = 'a'
};

// clang-format off
struct LinearHighlight{ CellLocation from; CellLocation to; };
struct RectangularHighlight { CellLocation from; CellLocation to; };
// clang-format on

using HighlightRange = std::variant<LinearHighlight, RectangularHighlight>;

enum class SearchEditMode
{
    Disabled,
    Enabled,
    ExternallyEnabled,
};

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

        virtual void execute(ViOperator op, ViMotion motion, unsigned count, char32_t lastChar = U'\0') = 0;
        virtual void moveCursor(ViMotion motion, unsigned count, char32_t lastChar = U'\0') = 0;
        virtual void select(TextObjectScope scope, TextObject textObject) = 0;
        virtual void yank(TextObjectScope scope, TextObject textObject) = 0;
        virtual void yank(ViMotion motion) = 0;
        virtual void paste(unsigned count, bool stripped) = 0;

        virtual void modeChanged(ViMode mode) = 0;

        virtual void searchStart() = 0;
        virtual void searchDone() = 0;
        virtual void searchCancel() = 0;
        virtual void updateSearchTerm(std::u32string const& text) = 0;

        virtual void scrollViewport(ScrollOffset delta) = 0;

        // Starts searching for the word under the cursor position in reverse order.
        // This is like pressing # in Vi.
        virtual void reverseSearchCurrentWord() = 0;

        // Toggle line mark (see LineFlags::Flagged).
        virtual void toggleLineMark() = 0;

        // Similar to reverse search, but searching forward.
        virtual void searchCurrentWord() = 0;
    };

    ViInputHandler(Executor& theExecutor, ViMode initialMode);

    bool sendKeyPressEvent(Key key, Modifier modifier) override;
    bool sendCharPressEvent(char32_t ch, Modifier modifier) override;

    void setMode(ViMode mode);
    void toggleMode(ViMode mode);
    [[nodiscard]] ViMode mode() const noexcept { return _viMode; }

    [[nodiscard]] bool isVisualMode() const noexcept
    {
        switch (_viMode)
        {
            case ViMode::Insert:
            case ViMode::Normal: return false;
            case ViMode::Visual:
            case ViMode::VisualBlock:
            case ViMode::VisualLine: return true;
        }
        crispy::unreachable();
    }

    [[nodiscard]] CellLocationRange translateToCellRange(TextObjectScope scope,
                                                         TextObject textObject) const noexcept;

    [[nodiscard]] bool isEditingSearch() const noexcept
    {
        return _searchEditMode != SearchEditMode::Disabled;
    }

    void startSearchExternally();

  private:
    enum class ModeSelect
    {
        Normal,
        Visual
    };

    using CommandHandler = std::function<void()>;
    using CommandHandlerMap = crispy::TrieMap<std::string, CommandHandler>;

    void registerAllCommands();
    void registerCommand(ModeSelect modes, std::string_view command, CommandHandler handler);
    void registerCommand(ModeSelect modes,
                         std::vector<std::string_view> const& commands,
                         CommandHandler handler);
    void appendModifierToPendingInput(Modifier modifier);
    [[nodiscard]] bool handlePendingInput();
    void clearPendingInput();
    [[nodiscard]] unsigned count() const noexcept { return _count ? _count : 1; }

    bool parseCount(char32_t ch, Modifier modifier);
    bool parseTextObject(char32_t ch, Modifier modifier);
    bool handleSearchEditor(char32_t ch, Modifier modifier);
    bool handleModeSwitches(char32_t ch, Modifier modifier);
    void startSearch();

    ViMode _viMode = ViMode::Normal;

    SearchEditMode _searchEditMode = SearchEditMode::Disabled;
    bool _searchExternallyActivated = false;
    std::u32string _searchTerm;

    std::string _pendingInput;
    CommandHandlerMap _normalMode;
    CommandHandlerMap _visualMode;
    unsigned _count = 0;
    char32_t _lastChar = 0;
    Executor& _executor;
};

} // namespace terminal

namespace fmt // {{{
{

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
            case TextObjectScope::Inner: return fmt::format_to(ctx.out(), "inner");
            case TextObjectScope::A: return fmt::format_to(ctx.out(), "a");
        }
        return fmt::format_to(ctx.out(), "({})", static_cast<unsigned>(scope));
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
            case TextObject::AngleBrackets: return fmt::format_to(ctx.out(), "AngleBrackets");
            case TextObject::BackQuotes: return fmt::format_to(ctx.out(), "BackQuotes");
            case TextObject::CurlyBrackets: return fmt::format_to(ctx.out(), "CurlyBrackets");
            case TextObject::DoubleQuotes: return fmt::format_to(ctx.out(), "DoubleQuotes");
            case TextObject::Paragraph: return fmt::format_to(ctx.out(), "Paragraph");
            case TextObject::RoundBrackets: return fmt::format_to(ctx.out(), "RoundBrackets");
            case TextObject::SingleQuotes: return fmt::format_to(ctx.out(), "SingleQuotes");
            case TextObject::SquareBrackets: return fmt::format_to(ctx.out(), "SquareBrackets");
            case TextObject::Word: return fmt::format_to(ctx.out(), "Word");
            case TextObject::BigWord: return fmt::format_to(ctx.out(), "BigWord");
        }
        return fmt::format_to(ctx.out(), "({})", static_cast<unsigned>(value));
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
            case ViOperator::MoveCursor: return fmt::format_to(ctx.out(), "MoveCursor");
            case ViOperator::Yank: return fmt::format_to(ctx.out(), "Yank");
            case ViOperator::Paste: return fmt::format_to(ctx.out(), "Paste");
            case ViOperator::PasteStripped: return fmt::format_to(ctx.out(), "PasteStripped");
            case ViOperator::ReverseSearchCurrentWord:
                return fmt::format_to(ctx.out(), "ReverseSearchCurrentWord");
        }
        return fmt::format_to(ctx.out(), "({})", static_cast<unsigned>(op));
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
            case ViMotion::Explicit: return fmt::format_to(ctx.out(), "Explicit");
            case ViMotion::Selection: return fmt::format_to(ctx.out(), "Selection");
            case ViMotion::FullLine: return fmt::format_to(ctx.out(), "FullLine");
            case ViMotion::CharLeft: return fmt::format_to(ctx.out(), "CharLeft");
            case ViMotion::CharRight: return fmt::format_to(ctx.out(), "CharRight");
            case ViMotion::ScreenColumn: return fmt::format_to(ctx.out(), "ScreenColumn");
            case ViMotion::FileBegin: return fmt::format_to(ctx.out(), "FileBegin");
            case ViMotion::FileEnd: return fmt::format_to(ctx.out(), "FileEnd");
            case ViMotion::LineBegin: return fmt::format_to(ctx.out(), "LineBegin");
            case ViMotion::LineTextBegin: return fmt::format_to(ctx.out(), "LineTextBegin");
            case ViMotion::LineDown: return fmt::format_to(ctx.out(), "LineDown");
            case ViMotion::LineEnd: return fmt::format_to(ctx.out(), "LineEnd");
            case ViMotion::LineUp: return fmt::format_to(ctx.out(), "LineUp");
            case ViMotion::LinesCenter: return fmt::format_to(ctx.out(), "LinesCenter");
            case ViMotion::PageDown: return fmt::format_to(ctx.out(), "PageDown");
            case ViMotion::PageUp: return fmt::format_to(ctx.out(), "PageUp");
            case ViMotion::PageTop: return fmt::format_to(ctx.out(), "PageTop");
            case ViMotion::PageBottom: return fmt::format_to(ctx.out(), "PageBottom");
            case ViMotion::ParagraphBackward: return fmt::format_to(ctx.out(), "ParagraphBackward");
            case ViMotion::ParagraphForward: return fmt::format_to(ctx.out(), "ParagraphForward");
            case ViMotion::ParenthesisMatching: return fmt::format_to(ctx.out(), "ParenthesisMatching");
            case ViMotion::SearchResultBackward: return fmt::format_to(ctx.out(), "SearchResultBackward");
            case ViMotion::SearchResultForward: return fmt::format_to(ctx.out(), "SearchResultForward");
            case ViMotion::WordBackward: return fmt::format_to(ctx.out(), "WordBackward");
            case ViMotion::WordEndForward: return fmt::format_to(ctx.out(), "WordEndForward");
            case ViMotion::WordForward: return fmt::format_to(ctx.out(), "WordForward");
            case ViMotion::BigWordBackward: return fmt::format_to(ctx.out(), "BigWordBackward");
            case ViMotion::BigWordEndForward: return fmt::format_to(ctx.out(), "BigWordEndForward");
            case ViMotion::BigWordForward: return fmt::format_to(ctx.out(), "BigWordForward");
            case ViMotion::TillBeforeCharRight: return fmt::format_to(ctx.out(), "TillBeforeCharRight");
            case ViMotion::TillAfterCharLeft: return fmt::format_to(ctx.out(), "TillAfterCharLeft");
            case ViMotion::ToCharRight: return fmt::format_to(ctx.out(), "ToCharRight");
            case ViMotion::ToCharLeft: return fmt::format_to(ctx.out(), "ToCharLeft");
            case ViMotion::RepeatCharMove: return fmt::format_to(ctx.out(), "RepeatCharMove");
            case ViMotion::RepeatCharMoveReverse: return fmt::format_to(ctx.out(), "RepeatCharMoveReverse");
            case ViMotion::GlobalCurlyCloseUp: return fmt::format_to(ctx.out(), "GlobalCurlyCloseUp");
            case ViMotion::GlobalCurlyCloseDown: return fmt::format_to(ctx.out(), "GlobalCurlyCloseDown");
            case ViMotion::GlobalCurlyOpenUp: return fmt::format_to(ctx.out(), "GlobalCurlyOpenUp");
            case ViMotion::GlobalCurlyOpenDown: return fmt::format_to(ctx.out(), "GlobalCurlyOpenDown");
            case ViMotion::LineMarkUp: return fmt::format_to(ctx.out(), "LineMarkUp");
            case ViMotion::LineMarkDown: return fmt::format_to(ctx.out(), "LineMarkDown");
        }
        return fmt::format_to(ctx.out(), "({})", static_cast<unsigned>(motion));
    }
};

} // namespace fmt
// }}}
