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

enum class vi_motion
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

enum class vi_operator
{
    MoveCursor,
    Yank = 'y',
    Paste = 'p',
    PasteStripped = 'P',
    ReverseSearchCurrentWord = '#',
};

enum class text_object
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

enum class text_object_scope
{
    Inner = 'i',
    A = 'a'
};

// clang-format off
struct linear_highlight{ cell_location from; cell_location to; };
struct rectangular_highlight { cell_location from; cell_location to; };
// clang-format on

using highlight_range = std::variant<linear_highlight, rectangular_highlight>;

enum class search_edit_mode
{
    Disabled,
    Enabled,
    ExternallyEnabled,
};

/**
 * ViInputHandler provides Vi-input handling.
 */
class vi_input_handler: public InputHandler
{
  public:
    class executor
    {
      public:
        virtual ~executor() = default;

        virtual void execute(vi_operator op, vi_motion motion, unsigned count, char32_t lastChar = U'\0') = 0;
        virtual void moveCursor(vi_motion motion, unsigned count, char32_t lastChar = U'\0') = 0;
        virtual void select(text_object_scope scope, text_object textObject) = 0;
        virtual void yank(text_object_scope scope, text_object textObject) = 0;
        virtual void yank(vi_motion motion) = 0;
        virtual void paste(unsigned count, bool stripped) = 0;

        virtual void modeChanged(vi_mode mode) = 0;

        virtual void searchStart() = 0;
        virtual void searchDone() = 0;
        virtual void searchCancel() = 0;
        virtual void updateSearchTerm(std::u32string const& text) = 0;

        virtual void scrollViewport(scroll_offset delta) = 0;

        // Starts searching for the word under the cursor position in reverse order.
        // This is like pressing # in Vi.
        virtual void reverseSearchCurrentWord() = 0;

        // Toggle line mark (see LineFlags::Flagged).
        virtual void toggleLineMark() = 0;

        // Similar to reverse search, but searching forward.
        virtual void searchCurrentWord() = 0;
    };

    vi_input_handler(executor& theExecutor, vi_mode initialMode);

    bool sendKeyPressEvent(key key, modifier modifier) override;
    bool sendCharPressEvent(char32_t ch, modifier modifier) override;

    void setMode(vi_mode mode);
    void toggleMode(vi_mode mode);
    [[nodiscard]] vi_mode mode() const noexcept { return _viMode; }

    [[nodiscard]] bool isVisualMode() const noexcept
    {
        switch (_viMode)
        {
            case vi_mode::Insert:
            case vi_mode::Normal: return false;
            case vi_mode::Visual:
            case vi_mode::VisualBlock:
            case vi_mode::VisualLine: return true;
        }
        crispy::unreachable();
    }

    [[nodiscard]] bool isEditingSearch() const noexcept
    {
        return _searchEditMode != search_edit_mode::Disabled;
    }

    void startSearchExternally();

  private:
    enum class mode_select
    {
        Normal,
        Visual
    };

    using command_handler = std::function<void()>;
    using command_handler_map = crispy::TrieMap<std::string, command_handler>;

    void registerAllCommands();
    void registerCommand(mode_select modes, std::string_view command, command_handler handler);
    void registerCommand(mode_select modes,
                         std::vector<std::string_view> const& commands,
                         command_handler const& handler);
    void appendModifierToPendingInput(modifier modifier);
    [[nodiscard]] bool handlePendingInput();
    void clearPendingInput();
    [[nodiscard]] unsigned count() const noexcept { return _count ? _count : 1; }

    bool parseCount(char32_t ch, modifier modifier);
    bool parseTextObject(char32_t ch, modifier modifier);
    bool handleSearchEditor(char32_t ch, modifier modifier);
    bool handleModeSwitches(char32_t ch, modifier modifier);
    void startSearch();

    vi_mode _viMode = vi_mode::Normal;

    search_edit_mode _searchEditMode = search_edit_mode::Disabled;
    bool _searchExternallyActivated = false;
    std::u32string _searchTerm;

    std::string _pendingInput;
    command_handler_map _normalMode;
    command_handler_map _visualMode;
    unsigned _count = 0;
    char32_t _lastChar = 0;
    executor& _executor;
};

} // namespace terminal

// {{{ fmtlib custom formatters
template <>
struct fmt::formatter<terminal::text_object_scope>: formatter<std::string_view>
{
    auto format(terminal::text_object_scope scope, format_context& ctx) -> format_context::iterator
    {
        using text_object_scope = terminal::text_object_scope;
        string_view name;
        switch (scope)
        {
            case text_object_scope::Inner: name = "inner"; break;
            case text_object_scope::A: name = "a"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::text_object>: formatter<std::string_view>
{
    auto format(terminal::text_object textObject, format_context& ctx) -> format_context::iterator
    {
        using text_object = terminal::text_object;
        string_view name;
        switch (textObject)
        {
            case text_object::AngleBrackets: name = "AngleBrackets"; break;
            case text_object::BackQuotes: name = "BackQuotes"; break;
            case text_object::CurlyBrackets: name = "CurlyBrackets"; break;
            case text_object::DoubleQuotes: name = "DoubleQuotes"; break;
            case text_object::LineMark: name = "LineMark"; break;
            case text_object::Paragraph: name = "Paragraph"; break;
            case text_object::RoundBrackets: name = "RoundBrackets"; break;
            case text_object::SingleQuotes: name = "SingleQuotes"; break;
            case text_object::SquareBrackets: name = "SquareBrackets"; break;
            case text_object::Word: name = "Word"; break;
            case text_object::BigWord: name = "BigWord"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::vi_operator>: formatter<std::string_view>
{
    auto format(terminal::vi_operator op, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        using terminal::vi_operator;
        switch (op)
        {
            case vi_operator::MoveCursor: name = "MoveCursor"; break;
            case vi_operator::Yank: name = "Yank"; break;
            case vi_operator::Paste: name = "Paste"; break;
            case vi_operator::PasteStripped: name = "PasteStripped"; break;
            case vi_operator::ReverseSearchCurrentWord: name = "ReverseSearchCurrentWord"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::vi_motion>: formatter<std::string_view>
{
    auto format(terminal::vi_motion motion, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        using terminal::vi_motion;
        switch (motion)
        {
            case vi_motion::Explicit: name = "Explicit"; break;
            case vi_motion::Selection: name = "Selection"; break;
            case vi_motion::FullLine: name = "FullLine"; break;
            case vi_motion::CharLeft: name = "CharLeft"; break;
            case vi_motion::CharRight: name = "CharRight"; break;
            case vi_motion::ScreenColumn: name = "ScreenColumn"; break;
            case vi_motion::FileBegin: name = "FileBegin"; break;
            case vi_motion::FileEnd: name = "FileEnd"; break;
            case vi_motion::LineBegin: name = "LineBegin"; break;
            case vi_motion::LineTextBegin: name = "LineTextBegin"; break;
            case vi_motion::LineDown: name = "LineDown"; break;
            case vi_motion::LineEnd: name = "LineEnd"; break;
            case vi_motion::LineUp: name = "LineUp"; break;
            case vi_motion::LinesCenter: name = "LinesCenter"; break;
            case vi_motion::PageDown: name = "PageDown"; break;
            case vi_motion::PageUp: name = "PageUp"; break;
            case vi_motion::PageTop: name = "PageTop"; break;
            case vi_motion::PageBottom: name = "PageBottom"; break;
            case vi_motion::ParagraphBackward: name = "ParagraphBackward"; break;
            case vi_motion::ParagraphForward: name = "ParagraphForward"; break;
            case vi_motion::ParenthesisMatching: name = "ParenthesisMatching"; break;
            case vi_motion::SearchResultBackward: name = "SearchResultBackward"; break;
            case vi_motion::SearchResultForward: name = "SearchResultForward"; break;
            case vi_motion::WordBackward: name = "WordBackward"; break;
            case vi_motion::WordEndForward: name = "WordEndForward"; break;
            case vi_motion::WordForward: name = "WordForward"; break;
            case vi_motion::BigWordBackward: name = "BigWordBackward"; break;
            case vi_motion::BigWordEndForward: name = "BigWordEndForward"; break;
            case vi_motion::BigWordForward: name = "BigWordForward"; break;
            case vi_motion::TillBeforeCharRight: name = "TillBeforeCharRight"; break;
            case vi_motion::TillAfterCharLeft: name = "TillAfterCharLeft"; break;
            case vi_motion::ToCharRight: name = "ToCharRight"; break;
            case vi_motion::ToCharLeft: name = "ToCharLeft"; break;
            case vi_motion::RepeatCharMove: name = "RepeatCharMove"; break;
            case vi_motion::RepeatCharMoveReverse: name = "RepeatCharMoveReverse"; break;
            case vi_motion::GlobalCurlyCloseUp: name = "GlobalCurlyCloseUp"; break;
            case vi_motion::GlobalCurlyCloseDown: name = "GlobalCurlyCloseDown"; break;
            case vi_motion::GlobalCurlyOpenUp: name = "GlobalCurlyOpenUp"; break;
            case vi_motion::GlobalCurlyOpenDown: name = "GlobalCurlyOpenDown"; break;
            case vi_motion::LineMarkUp: name = "LineMarkUp"; break;
            case vi_motion::LineMarkDown: name = "LineMarkDown"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
