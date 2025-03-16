// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputHandler.h>
#include <vtbackend/Selector.h>
#include <vtbackend/Settings.h>
#include <vtbackend/primitives.h>

#include <crispy/TrieMap.h>
#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <gsl/pointers>

namespace vtbackend
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

enum class ViMotion : uint8_t
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
    JumpToLastJumpPoint,   // '' or `` (jump to last jump)
    JumpToMarkBackward,    // <C-O>
    JumpToMarkForward,     // <C-I>
    CenterCursor,          // zz
};

enum class ViOperator : uint8_t
{
    MoveCursor,
    Yank = 'y',
    Paste = 'p',
    PasteStripped = 'P',
    ReverseSearchCurrentWord = '#',
    Open = 'o',
};

enum class TextObject : uint8_t
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

enum class TextObjectScope : uint8_t
{
    Inner = 'i',
    A = 'a'
};

// clang-format off
struct LinearHighlight{ CellLocation from; CellLocation to; };
struct RectangularHighlight { CellLocation from; CellLocation to; };
// clang-format on

using HighlightRange = std::variant<LinearHighlight, RectangularHighlight>;

enum class PromptMode : uint8_t
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
        virtual void open(TextObjectScope scope, TextObject textObject) = 0;
        virtual void paste(unsigned count, bool stripped) = 0;

        virtual void modeChanged(ViMode mode) = 0;

        virtual void searchStart() = 0;
        virtual void searchDone() = 0;
        virtual void searchCancel() = 0;
        virtual void updateSearchTerm(std::u32string const& text) = 0;

        virtual void promptStart(std::string const& query) = 0;
        virtual void promptDone() = 0;
        virtual void promptCancel() = 0;
        virtual void updatePromptText(std::string const& text) = 0;

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

    Handled sendKeyPressEvent(Key key, Modifiers modifiers, KeyboardEventType eventType) override;
    Handled sendCharPressEvent(char32_t ch, Modifiers modifiers, KeyboardEventType eventType) override;

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

    [[nodiscard]] bool isEditingSearch() const noexcept { return _searchEditMode != PromptMode::Disabled; }
    [[nodiscard]] bool isEditingPrompt() const noexcept { return _promptEditMode != PromptMode::Disabled; }

    void startSearchExternally();

    void setTabName(auto&& callback)
    {
        _promptText.clear();
        _executor->promptStart("Tab name: ");

        if (_viMode != ViMode::Insert)
            _promptEditMode = PromptMode::Enabled;
        else
        {
            _promptEditMode = PromptMode::ExternallyEnabled;
            setMode(ViMode::Normal);
            // ^^^ So that we can see the statusline (which contains the search edit field),
            // AND it's weird to be in insert mode while typing in the prompt term anyways.
        }

        _setTabNameCallback = callback;
    }

    void setSearchModeSwitch(bool enabled);
    void clearSearch();

  private:
    enum class ModeSelect : uint8_t
    {
        Normal,
        Visual
    };

    struct
    {
        bool fromSearchIntoInsertMode { true };
    } _settings;

    using CommandHandler = std::function<void()>;
    using CommandHandlerMap = crispy::trie_map<std::string, CommandHandler>;

    void registerAllCommands();
    void registerCommand(ModeSelect modes, std::string_view command, CommandHandler handler);
    void registerCommand(ModeSelect modes,
                         std::vector<std::string_view> const& commands,
                         CommandHandler const& handler);
    void appendModifierToPendingInput(Modifiers modifiers);
    void handlePendingInput();
    void clearPendingInput();
    [[nodiscard]] unsigned count() const noexcept { return _count ? _count : 1; }

    bool parseCount(char32_t ch, Modifiers modifiers);
    bool parseTextObject(char32_t ch, Modifiers modifiers);
    Handled handleSearchEditor(char32_t ch, Modifiers modifiers);
    Handled handlePromptEditor(char32_t ch, Modifiers modifiers);
    Handled handleModeSwitches(char32_t ch, Modifiers modifiers);
    void startSearch();

    ViMode _viMode = ViMode::Normal;

    PromptMode _searchEditMode = PromptMode::Disabled;
    PromptMode _promptEditMode = PromptMode::Disabled;
    bool _searchExternallyActivated = false;
    std::u32string _searchTerm;
    std::string _promptText;

    std::string _pendingInput;
    CommandHandlerMap _normalMode;
    CommandHandlerMap _visualMode;
    unsigned _count = 0;
    char32_t _lastChar = 0;
    gsl::not_null<Executor*> _executor;
    std::optional<std::function<void(std::string)>> _setTabNameCallback { std::nullopt };
};

} // namespace vtbackend

// {{{ fmtlib custom formatters
template <>
struct std::formatter<vtbackend::TextObjectScope>: formatter<std::string_view>
{
    auto format(vtbackend::TextObjectScope scope, auto& ctx) const
    {
        using TextObjectScope = vtbackend::TextObjectScope;
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
struct std::formatter<vtbackend::TextObject>: formatter<std::string_view>
{
    auto format(vtbackend::TextObject textObject, auto& ctx) const
    {
        using TextObject = vtbackend::TextObject;
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
struct std::formatter<vtbackend::ViOperator>: formatter<std::string_view>
{
    auto format(vtbackend::ViOperator op, auto& ctx) const
    {
        string_view name;
        using vtbackend::ViOperator;
        switch (op)
        {
            case ViOperator::MoveCursor: name = "MoveCursor"; break;
            case ViOperator::Yank: name = "Yank"; break;
            case ViOperator::Open: name = "Open"; break;
            case ViOperator::Paste: name = "Paste"; break;
            case ViOperator::PasteStripped: name = "PasteStripped"; break;
            case ViOperator::ReverseSearchCurrentWord: name = "ReverseSearchCurrentWord"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ViMotion>: formatter<std::string_view>
{
    auto format(vtbackend::ViMotion motion, auto& ctx) const
    {
        string_view name;
        using vtbackend::ViMotion;
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
            case ViMotion::JumpToLastJumpPoint: name = "JumpToLastJumpPoint"; break;
            case ViMotion::JumpToMarkBackward: name = "JumpToMarkUp"; break;
            case ViMotion::JumpToMarkForward: name = "JumpToMarkDown"; break;
            case ViMotion::GlobalCurlyCloseUp: name = "GlobalCurlyCloseUp"; break;
            case ViMotion::GlobalCurlyCloseDown: name = "GlobalCurlyCloseDown"; break;
            case ViMotion::GlobalCurlyOpenUp: name = "GlobalCurlyOpenUp"; break;
            case ViMotion::GlobalCurlyOpenDown: name = "GlobalCurlyOpenDown"; break;
            case ViMotion::LineMarkUp: name = "LineMarkUp"; break;
            case ViMotion::LineMarkDown: name = "LineMarkDown"; break;
            case ViMotion::CenterCursor: name = "CenterCursor"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
