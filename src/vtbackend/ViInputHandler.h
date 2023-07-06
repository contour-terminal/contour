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
    LineMark = 'm',       // im  am
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
                         CommandHandler const& handler);
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

// {{{ fmtlib custom formatters
template <>
struct fmt::formatter<terminal::TextObjectScope>: formatter<std::string_view>
{
    auto format(terminal::TextObjectScope scope, format_context& ctx) -> format_context::iterator
    {
        using TextObjectScope = terminal::TextObjectScope;
        string_view name;
        switch (scope)
        {
            case TextObjectScope::Inner: name = "inner"; break;
            case TextObjectScope::A: name = "a"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::TextObject>: formatter<std::string_view>
{
    auto format(terminal::TextObject textObject, format_context& ctx) -> format_context::iterator
    {
        using TextObject = terminal::TextObject;
        string_view name;
        switch (textObject)
        {
            case TextObject::AngleBrackets: name = "AngleBrackets"; break;
            case TextObject::BackQuotes: name = "BackQuotes"; break;
            case TextObject::CurlyBrackets: name = "CurlyBrackets"; break;
            case TextObject::DoubleQuotes: name = "DoubleQuotes"; break;
            case TextObject::LineMark: name = "LineMark"; break;
            case TextObject::Paragraph: name = "Paragraph"; break;
            case TextObject::RoundBrackets: name = "RoundBrackets"; break;
            case TextObject::SingleQuotes: name = "SingleQuotes"; break;
            case TextObject::SquareBrackets: name = "SquareBrackets"; break;
            case TextObject::Word: name = "Word"; break;
            case TextObject::BigWord: name = "BigWord"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::ViOperator>: formatter<std::string_view>
{
    auto format(terminal::ViOperator op, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        using terminal::ViOperator;
        switch (op)
        {
            case ViOperator::MoveCursor: name = "MoveCursor"; break;
            case ViOperator::Yank: name = "Yank"; break;
            case ViOperator::Paste: name = "Paste"; break;
            case ViOperator::PasteStripped: name = "PasteStripped"; break;
            case ViOperator::ReverseSearchCurrentWord: name = "ReverseSearchCurrentWord"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::ViMotion>: formatter<std::string_view>
{
    auto format(terminal::ViMotion motion, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        using terminal::ViMotion;
        switch (motion)
        {
            case ViMotion::Explicit: name = "Explicit"; break;
            case ViMotion::Selection: name = "Selection"; break;
            case ViMotion::FullLine: name = "FullLine"; break;
            case ViMotion::CharLeft: name = "CharLeft"; break;
            case ViMotion::CharRight: name = "CharRight"; break;
            case ViMotion::ScreenColumn: name = "ScreenColumn"; break;
            case ViMotion::FileBegin: name = "FileBegin"; break;
            case ViMotion::FileEnd: name = "FileEnd"; break;
            case ViMotion::LineBegin: name = "LineBegin"; break;
            case ViMotion::LineTextBegin: name = "LineTextBegin"; break;
            case ViMotion::LineDown: name = "LineDown"; break;
            case ViMotion::LineEnd: name = "LineEnd"; break;
            case ViMotion::LineUp: name = "LineUp"; break;
            case ViMotion::LinesCenter: name = "LinesCenter"; break;
            case ViMotion::PageDown: name = "PageDown"; break;
            case ViMotion::PageUp: name = "PageUp"; break;
            case ViMotion::PageTop: name = "PageTop"; break;
            case ViMotion::PageBottom: name = "PageBottom"; break;
            case ViMotion::ParagraphBackward: name = "ParagraphBackward"; break;
            case ViMotion::ParagraphForward: name = "ParagraphForward"; break;
            case ViMotion::ParenthesisMatching: name = "ParenthesisMatching"; break;
            case ViMotion::SearchResultBackward: name = "SearchResultBackward"; break;
            case ViMotion::SearchResultForward: name = "SearchResultForward"; break;
            case ViMotion::WordBackward: name = "WordBackward"; break;
            case ViMotion::WordEndForward: name = "WordEndForward"; break;
            case ViMotion::WordForward: name = "WordForward"; break;
            case ViMotion::BigWordBackward: name = "BigWordBackward"; break;
            case ViMotion::BigWordEndForward: name = "BigWordEndForward"; break;
            case ViMotion::BigWordForward: name = "BigWordForward"; break;
            case ViMotion::TillBeforeCharRight: name = "TillBeforeCharRight"; break;
            case ViMotion::TillAfterCharLeft: name = "TillAfterCharLeft"; break;
            case ViMotion::ToCharRight: name = "ToCharRight"; break;
            case ViMotion::ToCharLeft: name = "ToCharLeft"; break;
            case ViMotion::RepeatCharMove: name = "RepeatCharMove"; break;
            case ViMotion::RepeatCharMoveReverse: name = "RepeatCharMoveReverse"; break;
            case ViMotion::GlobalCurlyCloseUp: name = "GlobalCurlyCloseUp"; break;
            case ViMotion::GlobalCurlyCloseDown: name = "GlobalCurlyCloseDown"; break;
            case ViMotion::GlobalCurlyOpenUp: name = "GlobalCurlyOpenUp"; break;
            case ViMotion::GlobalCurlyOpenDown: name = "GlobalCurlyOpenDown"; break;
            case ViMotion::LineMarkUp: name = "LineMarkUp"; break;
            case ViMotion::LineMarkDown: name = "LineMarkDown"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
