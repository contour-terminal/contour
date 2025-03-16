// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/logging.h>

#include <crispy/TrieMap.h>
#include <crispy/assert.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <variant>

using std::pair;
using std::vector;
using namespace std::string_view_literals;

namespace vtbackend
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
        Modifiers modifiers;
        char32_t ch;

        [[nodiscard]] constexpr uint32_t code() const noexcept
        {
            return uint32_t(ch << 5) | uint32_t(modifiers.value() & 0b1'1111);
        }

        constexpr operator uint32_t() const noexcept
        {
            return uint32_t(ch << 5) | uint32_t(modifiers.value() & 0b1'1111);
        }
    };

    constexpr InputMatch operator"" _key(char ch)
    {
        return InputMatch { .modifiers = Modifier::None, .ch = static_cast<char32_t>(ch) };
    }

    constexpr InputMatch operator|(Modifier modifier, char ch) noexcept
    {
        return InputMatch { .modifiers = Modifiers { modifier }, .ch = static_cast<char32_t>(ch) };
    }
} // namespace

ViInputHandler::ViInputHandler(Executor& theExecutor, ViMode initialMode):
    _viMode { initialMode }, _executor { &theExecutor }
{
    registerAllCommands();
}

void ViInputHandler::registerAllCommands()
{
    auto constexpr ScopeMappings =
        std::array<std::pair<char, TextObjectScope>, 2> { { std::pair { 'i', TextObjectScope::Inner },
                                                            std::pair { 'a', TextObjectScope::A } } };

    auto constexpr MotionMappings = std::array<std::pair<std::string_view, ViMotion>, 48> { {
        // clang-format off
        { "$", ViMotion::LineEnd },
        { "%", ViMotion::ParenthesisMatching },
        { "0", ViMotion::LineBegin },
        { "<BS>", ViMotion::CharLeft },
        { "<NL>", ViMotion::LineDown },
        { "<Down>", ViMotion::LineDown },
        { "<End>", ViMotion::LineEnd },
        { "<Home>", ViMotion::LineBegin },
        { "<Left>", ViMotion::CharLeft },
        { "<PageDown>", ViMotion::PageDown },
        { "<PageUp>", ViMotion::PageUp },
        { "<Right>", ViMotion::CharRight },
        { "<Space>", ViMotion::CharRight },
        { "<Up>", ViMotion::LineUp },
        { "B", ViMotion::BigWordBackward },
        { "C-D", ViMotion::PageDown },
        { "C-U", ViMotion::PageUp },
        { "E", ViMotion::BigWordEndForward },
        { "G", ViMotion::FileEnd },
        { "H", ViMotion::PageTop },
        { "L", ViMotion::PageBottom },
        { "M", ViMotion::LinesCenter },
        { "N", ViMotion::SearchResultBackward },
        { "W", ViMotion::BigWordForward },
        { "[[", ViMotion::GlobalCurlyOpenUp },
        { "[]", ViMotion::GlobalCurlyCloseUp },
        { "[m", ViMotion::LineMarkUp },
        { "][", ViMotion::GlobalCurlyCloseDown },
        { "]]", ViMotion::GlobalCurlyOpenDown },
        { "]m", ViMotion::LineMarkDown },
        { "^", ViMotion::LineTextBegin },
        { "b", ViMotion::WordBackward },
        { "e", ViMotion::WordEndForward },
        { "gg", ViMotion::FileBegin },
        { "h", ViMotion::CharLeft },
        { "j", ViMotion::LineDown },
        { "k", ViMotion::LineUp },
        { "l", ViMotion::CharRight },
        { "n", ViMotion::SearchResultForward },
        { "w", ViMotion::WordForward },
        { "{", ViMotion::ParagraphBackward },
        { "|", ViMotion::ScreenColumn },
        { "}", ViMotion::ParagraphForward },
        { "''",ViMotion::JumpToLastJumpPoint },
        { "``",ViMotion::JumpToLastJumpPoint },
        { "C-O",ViMotion::JumpToMarkBackward },
        { "C-I",ViMotion::JumpToMarkForward },
        { "zz",ViMotion::CenterCursor },
        // clang-format on
    } };

    auto constexpr TextObjectMappings = std::array<std::pair<char, TextObject>, 15> { {
        { '"', TextObject::DoubleQuotes },
        { 'm', TextObject::LineMark },
        { '(', TextObject::RoundBrackets },
        { ')', TextObject::RoundBrackets },
        { '<', TextObject::AngleBrackets },
        { '>', TextObject::AngleBrackets },
        { 'W', TextObject::BigWord },
        { '[', TextObject::SquareBrackets },
        { ']', TextObject::SquareBrackets },
        { '\'', TextObject::SingleQuotes },
        { '`', TextObject::BackQuotes },
        { 'p', TextObject::Paragraph },
        { 'w', TextObject::Word },
        { '{', TextObject::CurlyBrackets },
        { '}', TextObject::CurlyBrackets },
    } };

    // normal mode and visual mode
    // clang-format off
    for (auto const modeSelect: { ModeSelect::Normal, ModeSelect::Visual })
    {
        for (auto const& [motionChar, motion]: MotionMappings)
            registerCommand(
                modeSelect, motionChar, [this, motion = motion]() { _executor->moveCursor(motion, count()); });

        registerCommand(modeSelect, "J", [this]() { _executor->scrollViewport(ScrollOffset(-1)); _executor->moveCursor(ViMotion::LineDown, 1);});
        registerCommand(modeSelect, "K", [this]() { _executor->scrollViewport(ScrollOffset(+1)); _executor->moveCursor(ViMotion::LineUp, 1);});
        registerCommand(modeSelect, "C-E", [this]() { _executor->scrollViewport(ScrollOffset(-1)); _executor->moveCursor(ViMotion::LineDown, 1);});
        registerCommand(modeSelect, "C-Y", [this]() { _executor->scrollViewport(ScrollOffset(+1)); _executor->moveCursor(ViMotion::LineUp, 1);});

        registerCommand(modeSelect, "t.", [this]() { _executor->moveCursor(ViMotion::TillBeforeCharRight, count(), _lastChar); });
        registerCommand(modeSelect, "T.", [this]() { _executor->moveCursor(ViMotion::TillAfterCharLeft, count(), _lastChar); });
        registerCommand(modeSelect, "f.", [this]() { _executor->moveCursor(ViMotion::ToCharRight, count(), _lastChar); });
        registerCommand(modeSelect, "F.", [this]() { _executor->moveCursor(ViMotion::ToCharLeft, count(), _lastChar); });
        registerCommand(modeSelect, ";", [this]() { _executor->moveCursor(ViMotion::RepeatCharMove, count()); });
        registerCommand(modeSelect, ",", [this]() { _executor->moveCursor(ViMotion::RepeatCharMoveReverse, count()); });
    }

    registerCommand(ModeSelect::Normal, "A", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "I", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "a", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "i", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "<Insert>", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "<Escape>", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "v", [this]() { toggleMode(ViMode::Visual); });
    registerCommand(ModeSelect::Normal, "V", [this]() { toggleMode(ViMode::VisualLine); });
    registerCommand(ModeSelect::Normal, "C-V", [this]() { toggleMode(ViMode::VisualBlock); });
    registerCommand(ModeSelect::Normal, "/", [this]() { startSearch(); });
    registerCommand(ModeSelect::Normal, "#", [this]() { _executor->reverseSearchCurrentWord(); });
    registerCommand(ModeSelect::Normal, "mm", [this]() { _executor->toggleLineMark(); });
    registerCommand(ModeSelect::Normal, "*", [this]() { _executor->searchCurrentWord(); });
    registerCommand(ModeSelect::Normal, "p", [this]() { _executor->paste(count(), false); });
    registerCommand(ModeSelect::Normal, "P", [this]() { _executor->paste(count(), true); });

    for (auto&& [theKey, viOperator]: { pair { 'y', ViOperator::Yank }, pair { 'o', ViOperator::Open } })
    {
        // This is solely a workaround for Apple Clang and FreeBSDs Clang.
        char const key = theKey;
        ViOperator const op = viOperator;

        // operate on the full line, with yy or oo.
        registerCommand(ModeSelect::Normal,
                        std::format("{}{}", key, key),
                        [this, op]() { _executor->execute(op, ViMotion::FullLine, count()); });

        for (auto && [motionChars, motion]: MotionMappings)
        {
            // Passing motion as motion=motion (new variable) is yet another workaround for Clang 15 (Ubuntu) this time.
            registerCommand(ModeSelect::Normal,
                            std::format("{}{}", key, motionChars),
                            [this, op, motion=motion]() { _executor->execute(op, motion, count()); });
        }

        auto const s3 = [key](char ch) { return std::format("{}{}.", key, ch); };
        registerCommand(ModeSelect::Normal, s3('t'), [this, op]() { _executor->execute(op, ViMotion::TillBeforeCharRight, count(), _lastChar); });
        registerCommand(ModeSelect::Normal, s3('T'), [this, op]() { _executor->execute(op, ViMotion::TillAfterCharLeft, count(), _lastChar); });
        registerCommand(ModeSelect::Normal, s3('f'), [this, op]() { _executor->execute(op, ViMotion::ToCharRight, count(), _lastChar); });
        registerCommand(ModeSelect::Normal, s3('F'), [this, op]() { _executor->execute(op, ViMotion::ToCharLeft, count(), _lastChar); });
    }
    // clang-format on

    for (auto const& [scopeChar, scope]: ScopeMappings)
    {
        for (auto const& [objectChar, obj]: TextObjectMappings)
        {
            registerCommand(ModeSelect::Normal,
                            std::format("y{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor->yank(scope, obj); });
            registerCommand(ModeSelect::Normal,
                            std::format("o{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor->open(scope, obj); });
        }
    }

    // visual mode
    registerCommand(ModeSelect::Visual, "/", [this]() { startSearch(); });
    registerCommand(ModeSelect::Visual, "y", [this]() {
        _executor->execute(ViOperator::Yank, ViMotion::Selection, count());
    });
    registerCommand(ModeSelect::Visual, "o", [this]() {
        _executor->execute(ViOperator::Open, ViMotion::Selection, count());
    });
    registerCommand(ModeSelect::Visual, "v", [this]() { toggleMode(ViMode::Normal); });
    registerCommand(ModeSelect::Visual, "V", [this]() { toggleMode(ViMode::VisualLine); });
    registerCommand(ModeSelect::Visual, "C-V", [this]() { toggleMode(ViMode::VisualBlock); });
    registerCommand(ModeSelect::Visual, "<ESC>", [this]() { setMode(ViMode::Normal); });
    for (auto const& [scopeChar, scope]: ScopeMappings)
        for (auto const& [objectChar, obj]: TextObjectMappings)
            registerCommand(ModeSelect::Visual,
                            std::format("{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor->select(scope, obj); });
}

void ViInputHandler::registerCommand(ModeSelect modes,
                                     std::vector<std::string_view> const& commands,
                                     CommandHandler const& handler)
{
    for (auto const& command: commands)
        registerCommand(modes, command, handler);
}

void ViInputHandler::registerCommand(ModeSelect modes, std::string_view command, CommandHandler handler)
{
    Require(!!handler);

    auto commandStr = crispy::replace(std::string(command.data(), command.size()), "<Space>", " ");

    inputLog()(
        "Registering command: {} in mode: {}", commandStr, modes == ModeSelect::Normal ? "Normal" : "Visual");

    switch (modes)
    {
        case ModeSelect::Normal: {
            Require(!_normalMode.contains(commandStr));
            _normalMode.insert(commandStr, std::move(handler));
            break;
        }
        case ModeSelect::Visual: {
            Require(!_visualMode.contains(commandStr));
            _visualMode.insert(commandStr, std::move(handler));
            break;
        }
        default: crispy::unreachable();
    }
}

void ViInputHandler::appendModifierToPendingInput(Modifiers modifiers)
{
    if (modifiers & Modifier::Super)
        // Super key is usually also named as Meta, conflicting with the actual Meta key.
        _pendingInput += "M-";
    if (modifiers & Modifier::Alt)
        _pendingInput += "A-";
    if (modifiers & Modifier::Shift)
        _pendingInput += "S-";
    if (modifiers & Modifier::Control)
        _pendingInput += "C-";
}

void ViInputHandler::handlePendingInput()
{
    assert(!_pendingInput.empty());

    auto constexpr TrieMapAllowWildcardDot = true;

    CommandHandlerMap const& mapping = isVisualMode() ? _visualMode : _normalMode;
    auto const mappingResult = mapping.search(_pendingInput, TrieMapAllowWildcardDot);
    if (std::holds_alternative<crispy::exact_match<CommandHandler>>(mappingResult))
    {
        inputLog()("Executing handler for: {}{}", _count ? std::format("{} ", _count) : "", _pendingInput);
        _lastChar =
            unicode::convert_to<char32_t>(std::string_view(_pendingInput.data(), _pendingInput.size()))
                .back();
        std::get<crispy::exact_match<CommandHandler>>(mappingResult).value();
        clearPendingInput();
    }
    else if (std::holds_alternative<crispy::no_match>(mappingResult))
    {
        inputLog()("Invalid command: {}", _pendingInput);
        clearPendingInput();
    }
    else
    {
        inputLog()("Incomplete input: {}", _pendingInput);
    }
}

void ViInputHandler::clearPendingInput()
{
    inputLog()("Resetting pending input: {}", _pendingInput);
    _count = 0;
    _pendingInput.clear();
}

void ViInputHandler::setMode(ViMode theMode)
{
    if (_viMode == theMode)
        return;

    _viMode = theMode;
    clearPendingInput();

    _executor->modeChanged(theMode);

    // clear search term when switching to insert mode
    if (_viMode == ViMode::Insert)
        clearSearch();
}

Handled ViInputHandler::sendKeyPressEvent(Key key, Modifiers modifiers, KeyboardEventType eventType)
{
    if (eventType == KeyboardEventType::Release)
        return Handled { true };

    if (_promptEditMode != PromptMode::Disabled)
    {
        // TODO: support cursor movements.
        switch (key)
        {
            case Key::Backspace: return handlePromptEditor('\x08', modifiers);
            case Key::Enter: return handlePromptEditor('\x0D', modifiers);
            case Key::Escape: return handlePromptEditor('\x1B', modifiers);
            default: break;
        }
        return Handled { true };
    }

    if (_searchEditMode != PromptMode::Disabled)
    {
        // TODO: support cursor movements.
        switch (key)
        {
            case Key::Backspace: return handleSearchEditor('\x08', modifiers);
            case Key::Enter: return handleSearchEditor('\x0D', modifiers);
            case Key::Escape: return handleSearchEditor('\x1B', modifiers);
            default: break;
        }
        return Handled { true };
    }

    // clang-format off
    switch (_viMode)
    {
        case ViMode::Insert:
            // In insert mode we do not handle any key events here.
            // The terminal will handle them and send them to the application.
            return Handled { false };
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock:
            if (key == Key::Escape && modifiers.none())
            {
                clearPendingInput();
                setMode(ViMode::Normal);
                return Handled { false };
            }
            [[fallthrough]];
        case ViMode::Normal:
            // We keep on handling key events below.
            break;
    }
    // clang-format on

    auto const charMappings = std::array<std::pair<Key, char32_t>, 12> { {
        { Key::Numpad_0, '0' },
        { Key::Numpad_1, '1' },
        { Key::Numpad_2, '2' },
        { Key::Numpad_3, '3' },
        { Key::Numpad_4, '4' },
        { Key::Numpad_5, '5' },
        { Key::Numpad_6, '6' },
        { Key::Numpad_7, '7' },
        { Key::Numpad_8, '8' },
        { Key::Numpad_9, '9' },
        { Key::Backspace, '\b' },
        { Key::Enter, '\n' },
    } };

    for (auto const& [mappedKey, mappedText]: charMappings)
        if (key == mappedKey)
            return sendCharPressEvent(mappedText, modifiers, eventType);

    if (modifiers.any())
        return Handled { true };

    auto const keyMappings = std::vector<std::pair<Key, std::string_view>> { {
        { Key::DownArrow, "<Down>" },
        { Key::LeftArrow, "<Left>" },
        { Key::RightArrow, "<Right>" },
        { Key::UpArrow, "<Up>" },
        { Key::Insert, "<Insert>" },
        { Key::Delete, "<Delete>" },
        { Key::Home, "<Home>" },
        { Key::End, "<End>" },
        { Key::PageUp, "<PageUp>" },
        { Key::PageDown, "<PageDown>" },
        { Key::Escape, "<Escape>" },
    } };

    for (auto const& [mappedKey, mappedText]: keyMappings)
        if (key == mappedKey)
        {
            _pendingInput += mappedText;
            break;
        }

    if (_pendingInput.empty())
    {
        errorLog()("ViInputHandler: Unhandled key: {} ({})", key, modifiers);
        return Handled { true };
    }

    handlePendingInput();
    return Handled { true };
}

void ViInputHandler::startSearchExternally()
{
    _searchTerm.clear();
    _executor->searchStart();

    if (_viMode != ViMode::Insert)
        _searchEditMode = PromptMode::Enabled;
    else
    {
        _searchEditMode = PromptMode::ExternallyEnabled;
        setMode(ViMode::Normal);
        // ^^^ So that we can see the statusline (which contains the search edit field),
        // AND it's weird to be in insert mode while typing in the search term anyways.
    }
}

auto handleEditor(char32_t ch,
                  Modifiers modifiers,
                  auto& where,
                  PromptMode& promptEditMode,
                  auto& settings,
                  auto setViMode,
                  auto cancel,
                  auto done,
                  auto update)
{
    switch (InputMatch { .modifiers = modifiers, .ch = ch })
    {
        case '\x1B'_key: {
            where.clear();
            if (promptEditMode == PromptMode::ExternallyEnabled)
                setViMode(ViMode::Insert);
            promptEditMode = PromptMode::Enabled;
            cancel();
            update(where);
        }
        break;
        case '\x0D'_key: {
            if (settings.fromSearchIntoInsertMode && promptEditMode == PromptMode::ExternallyEnabled)
                setViMode(ViMode::Insert);
            promptEditMode = PromptMode::Disabled;
            done();
        }
        break;
        case '\x08'_key:
        case '\x7F'_key:
            if (!where.empty())
                where.resize(where.size() - 1);
            update(where);
            break;
        case Modifier::Control | 'L':
        case Modifier::Control | 'U':
            where.clear();
            update(where);
            break;
        case Modifier::Control | 'A': // TODO: move cursor to BOL
        case Modifier::Control | 'E': // TODO: move cursor to EOL
        default:
            if (ch >= 0x20 && modifiers.without(Modifier::Shift).none())
            {
                where += ch;
                update(where);
            }
            else
                errorLog()("ViInputHandler: Receiving control code {}+0x{:02X} in search mode. Ignoring.",
                           modifiers,
                           (unsigned) ch);
    }
}

Handled ViInputHandler::handleSearchEditor(char32_t ch, Modifiers modifiers)
{
    assert(_searchEditMode != PromptMode::Disabled);

    handleEditor(
        ch,
        modifiers,
        _searchTerm,
        _searchEditMode,
        _settings,
        [&](auto mode) { setMode(mode); },
        [&]() { _executor->searchCancel(); },
        [&]() { _executor->searchDone(); },
        [&](const auto& val) { _executor->updateSearchTerm(val); });

    return Handled { true };
}

Handled ViInputHandler::handlePromptEditor(char32_t ch, Modifiers modifiers)
{
    assert(_promptEditMode != PromptMode::Disabled);

    handleEditor(
        ch,
        modifiers,
        _promptText,
        _promptEditMode,
        _settings,
        [&](auto mode) { setMode(mode); },
        [&]() { _executor->promptCancel(); },
        [&]() {
            _executor->promptDone();
            if (_setTabNameCallback)
            {
                _setTabNameCallback.value()(_promptText);
                setMode(ViMode::Insert);
                _setTabNameCallback = std::nullopt;
            }
        },
        [&](const auto& val) { _executor->updatePromptText(val); });

    return Handled { true };
}

Handled ViInputHandler::sendCharPressEvent(char32_t ch, Modifiers modifiers, KeyboardEventType eventType)
{
    if (eventType == KeyboardEventType::Release)
        return Handled { true };

    if (_searchEditMode != PromptMode::Disabled)
        return handleSearchEditor(ch, modifiers);

    if (_promptEditMode != PromptMode::Disabled)
        return handlePromptEditor(ch, modifiers);

    if (_viMode == ViMode::Insert)
        return Handled { false };

    if (ch == 033 && modifiers.none())
    {
        clearPendingInput();
        setMode(ViMode::Normal);
        return Handled { true };
    }

    if (parseCount(ch, modifiers))
        return Handled { true };

    appendModifierToPendingInput(ch > 0x20 ? modifiers.without(Modifier::Shift) : modifiers);

    if (ch == '\033')
        _pendingInput += "<ESC>";
    else if (ch == '\b')
        _pendingInput += "<BS>";
    else if (ch == '\n' || ch == '\r')
        _pendingInput += "<NL>";
    else
        _pendingInput += unicode::convert_to<char>(ch);

    if (_pendingInput.empty())
    {
        errorLog()("ViInputHandler: Unhandled char: {} ({})", static_cast<uint32_t>(ch), modifiers);
        return Handled { false };
    }

    if (!_pendingInput.empty())
        handlePendingInput();
    else
        errorLog()("ViInputHandler: Unhandled character: {} ({})", (unsigned) ch, modifiers);

    return Handled { true };
}

bool ViInputHandler::parseCount(char32_t ch, Modifiers modifiers)
{
    if (!modifiers.none())
        return false;

    switch (ch)
    {
        case '0':
            if (!_count)
                return false;
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
        default:
            //.
            return false;
    }
}

void ViInputHandler::startSearch()
{
    _searchEditMode = PromptMode::Enabled;
    _executor->searchStart();
}

void ViInputHandler::toggleMode(ViMode newMode)
{
    setMode(newMode != _viMode ? newMode : ViMode::Normal);
}

void ViInputHandler::setSearchModeSwitch(bool enabled)
{
    _settings.fromSearchIntoInsertMode = enabled;
}

void ViInputHandler::clearSearch()
{
    _searchTerm.clear();
    _executor->updateSearchTerm(_searchTerm);
}

} // namespace vtbackend
