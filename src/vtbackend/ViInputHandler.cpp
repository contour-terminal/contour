// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/logging.h>

#include <crispy/TrieMap.h>
#include <crispy/assert.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <variant>

using std::nullopt;
using std::optional;
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
    _viMode { initialMode }, _executor { &theExecutor }
{
    registerAllCommands();
}

void ViInputHandler::registerAllCommands()
{
    auto constexpr ScopeMappings =
        std::array<std::pair<char, TextObjectScope>, 2> { { std::pair { 'i', TextObjectScope::Inner },
                                                            std::pair { 'a', TextObjectScope::A } } };

    auto constexpr MotionMappings = std::array<std::pair<std::string_view, ViMotion>, 43> { {
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

        auto const s1 = [](char ch) { return std::string(1, ch); };
        registerCommand(ModeSelect::Normal, s1(toupper(key)), [this, op]() { _executor->execute(op, ViMotion::FullLine, count()); });

        auto const s2 = [key](char ch) { return fmt::format("{}{}", key, ch); };
        registerCommand(ModeSelect::Normal, s2(key), [this, op]() { _executor->execute(op, ViMotion::FullLine, count()); });
        registerCommand(ModeSelect::Normal, s2('b'), [this, op]() { _executor->execute(op, ViMotion::WordBackward, count()); });
        registerCommand(ModeSelect::Normal, s2('e'), [this, op]() { _executor->execute(op, ViMotion::WordEndForward, count()); });
        registerCommand(ModeSelect::Normal, s2('w'), [this, op]() { _executor->execute(op, ViMotion::WordForward, count()); });
        registerCommand(ModeSelect::Normal, s2('B'), [this, op]() { _executor->execute(op, ViMotion::BigWordBackward, count()); });
        registerCommand(ModeSelect::Normal, s2('E'), [this, op]() { _executor->execute(op, ViMotion::BigWordEndForward, count()); });
        registerCommand(ModeSelect::Normal, s2('W'), [this, op]() { _executor->execute(op, ViMotion::BigWordForward, count()); });

        auto const s3 = [key](char ch) { return fmt::format("{}{}.", key, ch); };
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
                            fmt::format("y{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor->yank(scope, obj); });
            registerCommand(ModeSelect::Normal,
                            fmt::format("o{}{}", scopeChar, objectChar),
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
                            fmt::format("{}{}", scopeChar, objectChar),
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

    switch (modes)
    {
        case ModeSelect::Normal: {
            inputLog()("Registering normal mode command: {}", command);
            Require(!_normalMode.contains(commandStr));
            _normalMode.insert(commandStr, std::move(handler));
            break;
        }
        case ModeSelect::Visual: {
            inputLog()("Registering visual mode command: {}", command);
            Require(!_visualMode.contains(commandStr));
            _visualMode.insert(commandStr, std::move(handler));
            break;
        }
        default: crispy::unreachable();
    }
}

void ViInputHandler::appendModifierToPendingInput(Modifier modifier)
{
    if (modifier.super())
        // Super key is usually also named as Meta, conflicting with the actual Meta key.
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
    if (_pendingInput.empty())
        return false;

    auto constexpr TrieMapAllowWildcardDot = true;

    CommandHandlerMap const& mapping = isVisualMode() ? _visualMode : _normalMode;
    auto const mappingResult = mapping.search(_pendingInput, TrieMapAllowWildcardDot);
    if (std::holds_alternative<crispy::exact_match<CommandHandler>>(mappingResult))
    {
        inputLog()("Executing handler for: {}{}", _count ? fmt::format("{} ", _count) : "", _pendingInput);
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

    return true;
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
}

bool ViInputHandler::sendKeyPressEvent(Key key, Modifier modifier)
{
    if (_searchEditMode != SearchEditMode::Disabled)
    {
        // TODO: support cursor movements.
        switch (key)
        {
            case Key::Backspace: return handleSearchEditor('\x08', modifier);
            case Key::Enter: return handleSearchEditor('\x0D', modifier);
            case Key::Escape: return handleSearchEditor('\x1B', modifier);
            default: break;
        }
        return true;
    }

    // clang-format off
    switch (_viMode)
    {
        case ViMode::Insert:
            return false;
        case ViMode::Normal:
            break;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock:
            if (key == Key::Escape && modifier.none())
            {
                clearPendingInput();
                setMode(ViMode::Normal);
                return true;
            }
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

    if (_pendingInput.empty())
    {
        errorLog()("ViInputHandler: Unhandled key: {} ({})", key, modifier);
        return false;
    }

    return handlePendingInput();
}

void ViInputHandler::startSearchExternally()
{
    _searchTerm.clear();
    _executor->searchStart();

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
            _executor->searchCancel();
            break;
        case '\x0D'_key:
            if (_searchEditMode == SearchEditMode::ExternallyEnabled)
                setMode(ViMode::Insert);
            _searchEditMode = SearchEditMode::Disabled;
            _executor->searchDone();
            break;
        case '\x08'_key:
        case '\x7F'_key:
            if (!_searchTerm.empty())
                _searchTerm.resize(_searchTerm.size() - 1);
            _executor->updateSearchTerm(_searchTerm);
            break;
        case Modifier::Control | 'L':
        case Modifier::Control | 'U':
            _searchTerm.clear();
            _executor->updateSearchTerm(_searchTerm);
            break;
        case Modifier::Control | 'A': // TODO: move cursor to BOL
        case Modifier::Control | 'E': // TODO: move cursor to EOL
        default:
            if (ch >= 0x20 && modifier.without(Modifier::Shift).none())
            {
                _searchTerm += ch;
                _executor->updateSearchTerm(_searchTerm);
            }
            else
                errorLog()("ViInputHandler: Receiving control code {}+0x{:02X} in search mode. Ignoring.",
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
    else if (ch == '\n' || ch == '\r')
        _pendingInput += "<NL>";
    else
        _pendingInput += unicode::convert_to<char>(ch);

    if (_pendingInput.empty())
    {
        errorLog()("ViInputHandler: Unhandled char: {} ({})", static_cast<uint32_t>(ch), modifier);
        return false;
    }

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
    _searchEditMode = SearchEditMode::Enabled;
    _executor->searchStart();
}

void ViInputHandler::toggleMode(ViMode newMode)
{
    setMode(newMode != _viMode ? newMode : ViMode::Normal);
}

} // namespace vtbackend
