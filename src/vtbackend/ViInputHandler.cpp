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

#include <variant>

#include <libunicode/convert.h>

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
    struct input_match
    {
        // ViMode mode; // TODO: ideally we also would like to match on input Mode
        modifier modifier;
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

    constexpr input_match operator"" _key(char ch)
    {
        return input_match { modifier::none, static_cast<char32_t>(ch) };
    }

    constexpr input_match operator|(modifier::key mod, char ch) noexcept
    {
        return input_match { modifier { mod }, (char32_t) ch };
    }
} // namespace

vi_input_handler::vi_input_handler(executor& theExecutor, vi_mode initialMode):
    _viMode { initialMode }, _executor { theExecutor }
{
    registerAllCommands();
}

void vi_input_handler::registerAllCommands()
{
    auto constexpr scopeMappings =
        std::array<std::pair<char, text_object_scope>, 2> { { std::pair { 'i', text_object_scope::Inner },
                                                              std::pair { 'a', text_object_scope::A } } };

    auto constexpr motionMappings = std::array<std::pair<std::string_view, vi_motion>, 43> { {
        // clang-format off
        { "$", vi_motion::LineEnd },
        { "%", vi_motion::ParenthesisMatching },
        { "0", vi_motion::LineBegin },
        { "<BS>", vi_motion::CharLeft },
        { "<NL>", vi_motion::LineDown },
        { "<Down>", vi_motion::LineDown },
        { "<End>", vi_motion::LineEnd },
        { "<Home>", vi_motion::LineBegin },
        { "<Left>", vi_motion::CharLeft },
        { "<PageDown>", vi_motion::PageDown },
        { "<PageUp>", vi_motion::PageUp },
        { "<Right>", vi_motion::CharRight },
        { "<Space>", vi_motion::CharRight },
        { "<Up>", vi_motion::LineUp },
        { "B", vi_motion::BigWordBackward },
        { "C-D", vi_motion::PageDown },
        { "C-U", vi_motion::PageUp },
        { "E", vi_motion::BigWordEndForward },
        { "G", vi_motion::FileEnd },
        { "H", vi_motion::PageTop },
        { "L", vi_motion::PageBottom },
        { "M", vi_motion::LinesCenter },
        { "N", vi_motion::SearchResultBackward },
        { "W", vi_motion::BigWordForward },
        { "[[", vi_motion::GlobalCurlyOpenUp },
        { "[]", vi_motion::GlobalCurlyCloseUp },
        { "[m", vi_motion::LineMarkUp },
        { "][", vi_motion::GlobalCurlyCloseDown },
        { "]]", vi_motion::GlobalCurlyOpenDown },
        { "]m", vi_motion::LineMarkDown },
        { "^", vi_motion::LineTextBegin },
        { "b", vi_motion::WordBackward },
        { "e", vi_motion::WordEndForward },
        { "gg", vi_motion::FileBegin },
        { "h", vi_motion::CharLeft },
        { "j", vi_motion::LineDown },
        { "k", vi_motion::LineUp },
        { "l", vi_motion::CharRight },
        { "n", vi_motion::SearchResultForward },
        { "w", vi_motion::WordForward },
        { "{", vi_motion::ParagraphBackward },
        { "|", vi_motion::ScreenColumn },
        { "}", vi_motion::ParagraphForward },
        // clang-format on
    } };

    auto constexpr textObjectMappings = std::array<std::pair<char, text_object>, 15> { {
        { '"', text_object::DoubleQuotes },
        { 'm', text_object::LineMark },
        { '(', text_object::RoundBrackets },
        { ')', text_object::RoundBrackets },
        { '<', text_object::AngleBrackets },
        { '>', text_object::AngleBrackets },
        { 'W', text_object::BigWord },
        { '[', text_object::SquareBrackets },
        { ']', text_object::SquareBrackets },
        { '\'', text_object::SingleQuotes },
        { '`', text_object::BackQuotes },
        { 'p', text_object::Paragraph },
        { 'w', text_object::Word },
        { '{', text_object::CurlyBrackets },
        { '}', text_object::CurlyBrackets },
    } };

    // normal mode and visual mode
    // clang-format off
    for (auto const modeSelect: { mode_select::Normal, mode_select::Visual })
    {
        for (auto const& [motionChar, motion]: motionMappings)
            registerCommand(
                modeSelect, motionChar, [this, motion = motion]() { _executor.moveCursor(motion, count()); });

        registerCommand(modeSelect, "J", [this]() { _executor.scrollViewport(scroll_offset(-1)); _executor.moveCursor(vi_motion::LineDown, 1);});
        registerCommand(modeSelect, "K", [this]() { _executor.scrollViewport(scroll_offset(+1)); _executor.moveCursor(vi_motion::LineUp, 1);});

        registerCommand(modeSelect, "t.", [this]() { _executor.moveCursor(vi_motion::TillBeforeCharRight, count(), _lastChar); });
        registerCommand(modeSelect, "T.", [this]() { _executor.moveCursor(vi_motion::TillAfterCharLeft, count(), _lastChar); });
        registerCommand(modeSelect, "f.", [this]() { _executor.moveCursor(vi_motion::ToCharRight, count(), _lastChar); });
        registerCommand(modeSelect, "F.", [this]() { _executor.moveCursor(vi_motion::ToCharLeft, count(), _lastChar); });
        registerCommand(modeSelect, ";", [this]() { _executor.moveCursor(vi_motion::RepeatCharMove, count()); });
        registerCommand(modeSelect, ",", [this]() { _executor.moveCursor(vi_motion::RepeatCharMoveReverse, count()); });
    }

    registerCommand(mode_select::Normal, "a", [this]() { setMode(vi_mode::Insert); });
    registerCommand(mode_select::Normal, "i", [this]() { setMode(vi_mode::Insert); });
    registerCommand(mode_select::Normal, "<Insert>", [this]() { setMode(vi_mode::Insert); });
    registerCommand(mode_select::Normal, "v", [this]() { toggleMode(vi_mode::Visual); });
    registerCommand(mode_select::Normal, "V", [this]() { toggleMode(vi_mode::VisualLine); });
    registerCommand(mode_select::Normal, "C-V", [this]() { toggleMode(vi_mode::VisualBlock); });
    registerCommand(mode_select::Normal, "/", [this]() { startSearch(); });
    registerCommand(mode_select::Normal, "#", [this]() { _executor.reverseSearchCurrentWord(); });
    registerCommand(mode_select::Normal, "mm", [this]() { _executor.toggleLineMark(); });
    registerCommand(mode_select::Normal, "*", [this]() { _executor.searchCurrentWord(); });
    registerCommand(mode_select::Normal, "p", [this]() { _executor.paste(count(), false); });
    registerCommand(mode_select::Normal, "P", [this]() { _executor.paste(count(), true); });

    registerCommand(mode_select::Normal, "Y", [this]() { _executor.execute(vi_operator::Yank, vi_motion::FullLine, count()); });
    registerCommand(mode_select::Normal, "yy", [this]() { _executor.execute(vi_operator::Yank, vi_motion::FullLine, count()); });
    registerCommand(mode_select::Normal, "yb", [this]() { _executor.execute(vi_operator::Yank, vi_motion::WordBackward, count()); });
    registerCommand(mode_select::Normal, "ye", [this]() { _executor.execute(vi_operator::Yank, vi_motion::WordEndForward, count()); });
    registerCommand(mode_select::Normal, "yw", [this]() { _executor.execute(vi_operator::Yank, vi_motion::WordForward, count()); });
    registerCommand(mode_select::Normal, "yB", [this]() { _executor.execute(vi_operator::Yank, vi_motion::BigWordBackward, count()); });
    registerCommand(mode_select::Normal, "yE", [this]() { _executor.execute(vi_operator::Yank, vi_motion::BigWordEndForward, count()); });
    registerCommand(mode_select::Normal, "yW", [this]() { _executor.execute(vi_operator::Yank, vi_motion::BigWordForward, count()); });
    registerCommand(mode_select::Normal, "yt.", [this]() { _executor.execute(vi_operator::Yank, vi_motion::TillBeforeCharRight, count(), _lastChar); });
    registerCommand(mode_select::Normal, "yT.", [this]() { _executor.execute(vi_operator::Yank, vi_motion::TillAfterCharLeft, count(), _lastChar); });
    registerCommand(mode_select::Normal, "yf.", [this]() { _executor.execute(vi_operator::Yank, vi_motion::ToCharRight, count(), _lastChar); });
    registerCommand(mode_select::Normal, "yF.", [this]() { _executor.execute(vi_operator::Yank, vi_motion::ToCharLeft, count(), _lastChar); });
    // clang-format on

    for (auto const& [scopeChar, scope]: scopeMappings)
        for (auto const& [objectChar, obj]: textObjectMappings)
            registerCommand(mode_select::Normal,
                            fmt::format("y{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor.yank(scope, obj); });

    // visual mode
    registerCommand(mode_select::Visual, "/", [this]() { startSearch(); });
    registerCommand(mode_select::Visual, "y", [this]() {
        _executor.execute(vi_operator::Yank, vi_motion::Selection, count());
    });
    registerCommand(mode_select::Visual, "v", [this]() { toggleMode(vi_mode::Normal); });
    registerCommand(mode_select::Visual, "V", [this]() { toggleMode(vi_mode::VisualLine); });
    registerCommand(mode_select::Visual, "C-V", [this]() { toggleMode(vi_mode::VisualBlock); });
    registerCommand(mode_select::Visual, "<ESC>", [this]() { setMode(vi_mode::Normal); });
    for (auto const& [scopeChar, scope]: scopeMappings)
        for (auto const& [objectChar, obj]: textObjectMappings)
            registerCommand(mode_select::Visual,
                            fmt::format("{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor.select(scope, obj); });
}

void vi_input_handler::registerCommand(mode_select modes,
                                       std::vector<std::string_view> const& commands,
                                       command_handler const& handler)
{
    for (auto const& command: commands)
        registerCommand(modes, command, handler);
}

void vi_input_handler::registerCommand(mode_select modes, std::string_view command, command_handler handler)
{
    Require(!!handler);

    auto commandStr = crispy::replace(std::string(command.data(), command.size()), "<Space>", " ");

    switch (modes)
    {
        case mode_select::Normal: {
            InputLog()("Registering normal mode command: {}", command);
            Require(!_normalMode.contains(commandStr));
            _normalMode.insert(commandStr, std::move(handler));
            break;
        }
        case mode_select::Visual: {
            InputLog()("Registering visual mode command: {}", command);
            Require(!_visualMode.contains(commandStr));
            _visualMode.insert(commandStr, std::move(handler));
            break;
        }
        default: crispy::unreachable();
    }
}

void vi_input_handler::appendModifierToPendingInput(modifier modifier)
{
    if (modifier.isMeta())
        _pendingInput += "M-";
    if (modifier.isAlt())
        _pendingInput += "A-";
    if (modifier.isShift())
        _pendingInput += "S-";
    if (modifier.isControlt())
        _pendingInput += "C-";
}

bool vi_input_handler::handlePendingInput()
{
    Require(!_pendingInput.empty());

    auto constexpr TrieMapAllowWildcardDot = true;

    command_handler_map const& mapping = isVisualMode() ? _visualMode : _normalMode;
    auto const mappingResult = mapping.search(_pendingInput, TrieMapAllowWildcardDot);
    if (std::holds_alternative<crispy::ExactMatch<command_handler>>(mappingResult))
    {
        InputLog()("Executing handler for: {}{}", _count ? fmt::format("{} ", _count) : "", _pendingInput);
        _lastChar =
            unicode::convert_to<char32_t>(std::string_view(_pendingInput.data(), _pendingInput.size()))
                .back();
        std::get<crispy::ExactMatch<command_handler>>(mappingResult).value();
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

void vi_input_handler::clearPendingInput()
{
    InputLog()("Resetting pending input: {}", _pendingInput);
    _count = 0;
    _pendingInput.clear();
}

void vi_input_handler::setMode(vi_mode theMode)
{
    if (_viMode == theMode)
        return;

    _viMode = theMode;
    clearPendingInput();

    _executor.modeChanged(theMode);
}

bool vi_input_handler::sendKeyPressEvent(key k, modifier modifier)
{
    if (_searchEditMode != search_edit_mode::Disabled)
    {
        // Do we want to do anything in here?
        // TODO: support cursor movements.
        errorlog()("ViInputHandler: Ignoring key input {}+{}.", modifier, k);
        return true;
    }

    // clang-format off
    switch (_viMode)
    {
        case vi_mode::Insert:
            return false;
        case vi_mode::Normal:
        case vi_mode::Visual:
        case vi_mode::VisualLine:
        case vi_mode::VisualBlock:
            break;
    }
    // clang-format on

    if (modifier.any())
        return true;

    auto const keyMappings = std::array<std::pair<key, std::string_view>, 10> { {
        { key::DownArrow, "<Down>" },
        { key::LeftArrow, "<Left>" },
        { key::RightArrow, "<Right>" },
        { key::UpArrow, "<Up>" },
        { key::Insert, "<Insert>" },
        { key::Delete, "<Delete>" },
        { key::Home, "<Home>" },
        { key::End, "<End>" },
        { key::PageUp, "<PageUp>" },
        { key::PageDown, "<PageDown>" },
    } };

    for (auto const& [mappedKey, mappedText]: keyMappings)
        if (k == mappedKey)
        {
            _pendingInput += mappedText;
            break;
        }

    return handlePendingInput();
}

void vi_input_handler::startSearchExternally()
{
    _searchTerm.clear();
    _executor.searchStart();

    if (_viMode != vi_mode::Insert)
        _searchEditMode = search_edit_mode::Enabled;
    else
    {
        _searchEditMode = search_edit_mode::ExternallyEnabled;
        setMode(vi_mode::Normal);
        // ^^^ So that we can see the statusline (which contains the search edit field),
        // AND it's weird to be in insert mode while typing in the search term anyways.
    }
}

bool vi_input_handler::handleSearchEditor(char32_t ch, modifier modifier)
{
    assert(_searchEditMode != SearchEditMode::Disabled);

    switch (input_match { modifier, ch })
    {
        case '\x1B'_key:
            _searchTerm.clear();
            if (_searchEditMode == search_edit_mode::ExternallyEnabled)
                setMode(vi_mode::Insert);
            _searchEditMode = search_edit_mode::Disabled;
            _executor.searchCancel();
            break;
        case '\x0D'_key:
            if (_searchEditMode == search_edit_mode::ExternallyEnabled)
                setMode(vi_mode::Insert);
            _searchEditMode = search_edit_mode::Disabled;
            _executor.searchDone();
            break;
        case '\x08'_key:
        case '\x7F'_key:
            if (!_searchTerm.empty())
                _searchTerm.resize(_searchTerm.size() - 1);
            _executor.updateSearchTerm(_searchTerm);
            break;
        case modifier::control | 'L':
        case modifier::control | 'U':
            _searchTerm.clear();
            _executor.updateSearchTerm(_searchTerm);
            break;
        case modifier::control | 'A': // TODO: move cursor to BOL
        case modifier::control | 'E': // TODO: move cursor to EOL
        default:
            if (ch >= 0x20 && modifier.without(modifier::shift).isNone())
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

bool vi_input_handler::sendCharPressEvent(char32_t ch, modifier modifier)
{
    if (_searchEditMode != search_edit_mode::Disabled)
        return handleSearchEditor(ch, modifier);

    if (_viMode == vi_mode::Insert)
        return false;

    if (ch == 033 && modifier.isNone())
    {
        clearPendingInput();
        setMode(vi_mode::Normal);
        return true;
    }

    if (parseCount(ch, modifier))
        return true;

    appendModifierToPendingInput(ch > 0x20 ? modifier.without(modifier::shift) : modifier);

    if (ch == '\033')
        _pendingInput += "<ESC>";
    else if (ch == '\b')
        _pendingInput += "<BS>";
    else if (ch == '\n' || ch == '\r')
        _pendingInput += "<NL>";
    else
        _pendingInput += unicode::convert_to<char>(ch);

    if (handlePendingInput())
        return true;

    return false;
}

bool vi_input_handler::parseCount(char32_t ch, modifier modifier)
{
    if (!modifier.isNone())
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

void vi_input_handler::startSearch()
{
    _searchEditMode = search_edit_mode::Enabled;
    _executor.searchStart();
}

void vi_input_handler::toggleMode(vi_mode newMode)
{
    setMode(newMode != _viMode ? newMode : vi_mode::Normal);
}

} // namespace terminal
