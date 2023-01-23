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

#include <variant>

using std::nullopt;
using std::optional;
using std::pair;
using std::vector;
using namespace std::string_view_literals;

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
} // namespace

ViInputHandler::ViInputHandler(Executor& theExecutor, ViMode initialMode):
    _viMode { initialMode }, _executor { theExecutor }
{
    registerAllCommands();
}

void ViInputHandler::registerAllCommands()
{
    auto constexpr scopeMappings =
        std::array<std::pair<char, TextObjectScope>, 2> { { std::pair { 'i', TextObjectScope::Inner },
                                                            std::pair { 'a', TextObjectScope::A } } };

    auto constexpr motionMappings = std::array<std::pair<std::string_view, ViMotion>, 40> { {
        // clang-format off
        { "$", ViMotion::LineEnd },
        { "%", ViMotion::ParenthesisMatching },
        { "0", ViMotion::LineBegin },
        { "<BS>", ViMotion::CharLeft },
        { "<Down>", ViMotion::LineDown },
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
        // clang-format on
    } };

    auto constexpr textObjectMappings = std::array<std::pair<char, TextObject>, 14> { {
        { '"', TextObject::DoubleQuotes },
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
    for (auto const modeSelect: { ModeSelect::Normal, ModeSelect::Visual })
        for (auto const& [motionChar, motion]: motionMappings)
            registerCommand(
                modeSelect, motionChar, [this, motion = motion]() { _executor.moveCursor(motion, count()); });

    // clang-format off
    // {{{ normal mode
    registerCommand(ModeSelect::Normal, "a", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "i", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "<Insert>", [this]() { setMode(ViMode::Insert); });
    registerCommand(ModeSelect::Normal, "v", [this]() { toggleMode(ViMode::Visual); });
    registerCommand(ModeSelect::Normal, "V", [this]() { toggleMode(ViMode::VisualLine); });
    registerCommand(ModeSelect::Normal, "C-V", [this]() { toggleMode(ViMode::VisualBlock); });
    registerCommand(ModeSelect::Normal, "/", [this]() { startSearch(); });
    registerCommand(ModeSelect::Normal, "#", [this]() { _executor.reverseSearchCurrentWord(); });
    registerCommand(ModeSelect::Normal, "*", [this]() { _executor.searchCurrentWord(); });
    registerCommand(ModeSelect::Normal, "p", [this]() { _executor.paste(count(), false); });
    registerCommand(ModeSelect::Normal, "P", [this]() { _executor.paste(count(), true); });

    registerCommand(ModeSelect::Normal, "Y", [this]() { _executor.execute(ViOperator::Yank, ViMotion::FullLine, count()); });
    registerCommand(ModeSelect::Normal, "yy", [this]() { _executor.execute(ViOperator::Yank, ViMotion::FullLine, count()); });
    registerCommand(ModeSelect::Normal, "yb", [this]() { _executor.execute(ViOperator::Yank, ViMotion::WordBackward, count()); });
    registerCommand(ModeSelect::Normal, "ye", [this]() { _executor.execute(ViOperator::Yank, ViMotion::WordEndForward, count()); });
    registerCommand(ModeSelect::Normal, "yw", [this]() { _executor.execute(ViOperator::Yank, ViMotion::WordForward, count()); });
    registerCommand(ModeSelect::Normal, "yB", [this]() { _executor.execute(ViOperator::Yank, ViMotion::BigWordBackward, count()); });
    registerCommand(ModeSelect::Normal, "yE", [this]() { _executor.execute(ViOperator::Yank, ViMotion::BigWordEndForward, count()); });
    registerCommand(ModeSelect::Normal, "yW", [this]() { _executor.execute(ViOperator::Yank, ViMotion::BigWordForward, count()); });
    // clang-format on

    for (auto const& [scopeChar, scope]: scopeMappings)
        for (auto const& [objectChar, obj]: textObjectMappings)
            registerCommand(ModeSelect::Normal,
                            fmt::format("y{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor.yank(scope, obj); });

    // visual mode
    registerCommand(ModeSelect::Visual, "/", [this]() { startSearch(); });
    registerCommand(ModeSelect::Visual, "y", [this]() {
        _executor.execute(ViOperator::Yank, ViMotion::Selection, count());
    });
    registerCommand(ModeSelect::Visual, "v", [this]() { toggleMode(ViMode::Normal); });
    registerCommand(ModeSelect::Visual, "V", [this]() { toggleMode(ViMode::VisualLine); });
    registerCommand(ModeSelect::Visual, "C-V", [this]() { toggleMode(ViMode::VisualBlock); });
    registerCommand(ModeSelect::Visual, "<ESC>", [this]() { setMode(ViMode::Normal); });
    for (auto const& [scopeChar, scope]: scopeMappings)
        for (auto const& [objectChar, obj]: textObjectMappings)
            registerCommand(ModeSelect::Visual,
                            fmt::format("{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor.select(scope, obj); });
}

void ViInputHandler::registerCommand(ModeSelect modes,
                                     std::vector<std::string_view> const& commands,
                                     CommandHandler handler)
{
    for (auto const& command: commands)
        registerCommand(modes, command, handler);
}

void ViInputHandler::registerCommand(ModeSelect modes, std::string_view command, CommandHandler handler)
{
    Require(!!handler);

    auto commandStr = std::string(command.data(), command.size());

    switch (modes)
    {
        case ModeSelect::Normal: {
            InputLog()("Registering normal mode command: {}", command);
            Require(!_normalMode.contains(commandStr));
            _normalMode.insert(std::move(commandStr), std::move(handler));
            break;
        }
        case ModeSelect::Visual: {
            InputLog()("Registering visual mode command: {}", command);
            Require(!_visualMode.contains(commandStr));
            _visualMode.insert(std::move(commandStr), std::move(handler));
            break;
        }
        default: crispy::unreachable();
    }
}

void ViInputHandler::appendModifierToPendingInput(Modifier modifier)
{
    if (modifier.meta())
        _pendingInput += "M-";
    if (modifier.alt())
        _pendingInput += "A-";
    if (modifier.shift())
        _pendingInput += "S-";
    if (modifier.control())
        _pendingInput += "C-";
}

bool ViInputHandler::handlePendingInput()
{
    CommandHandlerMap const& mapping = isVisualMode() ? _visualMode : _normalMode;
    auto const mappingResult = mapping.search(_pendingInput);
    if (std::holds_alternative<crispy::ExactMatch<CommandHandler>>(mappingResult))
    {
        InputLog()("Executing handler for: {}{}", _count ? fmt::format("{} ", _count) : "", _pendingInput);
        std::get<crispy::ExactMatch<CommandHandler>>(mappingResult).value();
        clearPendingInput();
    }
    else if (std::holds_alternative<crispy::NoMatch>(mappingResult))
    {
        InputLog()("Invalid command: {}", _pendingInput);
        clearPendingInput();
    }
    else
    {
        InputLog()("Incomplete input: {}", _pendingInput);
    }

    return true;
}

void ViInputHandler::clearPendingInput()
{
    InputLog()("Resetting pending input: {}", _pendingInput);
    _count = 0;
    _pendingInput.clear();
}

void ViInputHandler::setMode(ViMode theMode)
{
    if (_viMode == theMode)
        return;

    _viMode = theMode;
    clearPendingInput();

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

    auto const keyMappings = std::array<std::pair<Key, std::string_view>, 10> { {
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
    } };

    for (auto const& [mappedKey, mappedText]: keyMappings)
        if (key == mappedKey)
        {
            _pendingInput += mappedText;
            break;
        }

    return handlePendingInput();
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

    if (_viMode == ViMode::Insert)
        return false;

    if (ch == 033 && modifier.none())
    {
        clearPendingInput();
        setMode(ViMode::Normal);
        return true;
    }

    if (parseCount(ch, modifier))
        return true;

    appendModifierToPendingInput(ch > 0x20 ? modifier.without(Modifier::Shift) : modifier);

    if (ch == '\033')
        _pendingInput += "<ESC>";
    else if (ch == '\b')
        _pendingInput += "<BS>";
    else if (ch == ' ')
        _pendingInput += "<Space>";
    else
        _pendingInput += unicode::convert_to<char>(ch);

    if (handlePendingInput())
        return true;

    return false;
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

void ViInputHandler::startSearch()
{
    _searchEditMode = SearchEditMode::Enabled;
    _searchTerm.clear();
    _executor.searchStart();
}

void ViInputHandler::toggleMode(ViMode newMode)
{
    setMode(newMode != _viMode ? newMode : ViMode::Normal);
}

} // namespace terminal
