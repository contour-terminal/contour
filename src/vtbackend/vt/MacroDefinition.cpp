// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/MacroDefinition.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    /// The meanings of Pdt and Pen, each at the index of the number that names it.
    constexpr auto Deletions = std::array { MacroDeletion::SameId, MacroDeletion::All };
    constexpr auto Encodings = std::array { MacroEncoding::Text, MacroEncoding::HexPairs };

    /// The value of the hex digit @p ch, or std::nullopt if it is none.
    [[nodiscard]] constexpr std::optional<uint8_t> hexDigitValue(char ch) noexcept
    {
        if (ch >= '0' && ch <= '9')
            return static_cast<uint8_t>(ch - '0');
        if (ch >= 'A' && ch <= 'F')
            return static_cast<uint8_t>(ch - 'A' + 10);
        if (ch >= 'a' && ch <= 'f')
            return static_cast<uint8_t>(ch - 'a' + 10);
        return std::nullopt;
    }
} // namespace

std::expected<MacroDefinitionParameters, MacroDefinitionError> decodeMacroDefinitionParameters(
    unsigned id, unsigned deletion, unsigned encoding) noexcept
{
    if (id >= static_cast<unsigned>(MacroCount))
        return std::unexpected { MacroDefinitionError::InvalidId };
    if (deletion >= Deletions.size())
        return std::unexpected { MacroDefinitionError::InvalidDeletion };
    if (encoding >= Encodings.size())
        return std::unexpected { MacroDefinitionError::InvalidEncoding };
    return MacroDefinitionParameters { .id = static_cast<int>(id),
                                       .deletion = Deletions[deletion],
                                       .encoding = Encodings[encoding] };
}

MacroBodyCollector::MacroBodyCollector(MacroEncoding encoding, size_t room, Done done):
    _encoding { encoding }, _room { room }, _done { std::move(done) }
{
}

void MacroBodyCollector::pass(char ch)
{
    // Once the body is refused, the rest of the control string is consumed and nothing more.
    if (!_body)
        return;

    // DEC STD 070: "characters from 0/8 through 0/13 can be included to format the device control string,
    // but are not part of the macro definition." Windows Terminal leaves every C0 control out, in either
    // encoding. (ESC never gets here: it ends the control string.)
    //
    // DEL and the C1 range stay, in a text body too, though DEC STD 070 allows only "2/0 through 7/14 and
    // 10/00 through 15/15" there: Windows Terminal keeps them as well, and 8/0 to 9/15 are also UTF-8
    // continuation bytes, which a text body of UTF-8 cannot lose.
    if (static_cast<unsigned char>(ch) < 0x20)
        return;

    switch (_encoding)
    {
        case MacroEncoding::Text: append(ch); break;
        case MacroEncoding::HexPairs: passHex(ch); break;
    }
}

void MacroBodyCollector::passHex(char ch)
{
    // Hex pairs, and DEC STD 070's repeat sequence "! Pn ; D...D ;" among them. Anything else refuses the
    // whole definition: "If a non-hex pair is encountered that is not a valid repeat sequence, the entire
    // macro definition is ignored." The grammar is Windows Terminal's MacroBuffer::ParseDefinition, a repeat
    // within a repeat included, which it refuses.
    auto const digit = hexDigitValue(ch);
    switch (_hexState)
    {
        case HexState::FirstDigit:
            if (digit)
            {
                _highNibble = *digit;
                _hexState = HexState::SecondDigit;
            }
            else if (ch == '!' && !_pendingRepeat)
            {
                _repeatCount = 0;
                _hexState = HexState::RepeatCount;
            }
            else if (ch == ';' && _pendingRepeat)
                applyPendingRepeat();
            else
                refuse(MacroDefinitionError::NotHex);
            break;
        case HexState::SecondDigit:
            if (!digit)
            {
                refuse(MacroDefinitionError::NotHex);
                break;
            }
            _hexState = HexState::FirstDigit;
            append(static_cast<char>((_highNibble << 4) | *digit));
            break;
        case HexState::RepeatCount:
            if (ch >= '0' && ch <= '9')
                // Past the room a larger count makes no difference -- the copies outgrow it either way -- so
                // it stops there rather than overflowing.
                _repeatCount = std::min((_repeatCount * 10) + static_cast<size_t>(ch - '0'), _room + 1);
            else if (ch == ';')
            {
                _pendingRepeat = PendingRepeat { .count = _repeatCount, .start = _body->size() };
                _hexState = HexState::FirstDigit;
            }
            else
                refuse(MacroDefinitionError::NotHex);
            break;
    }
}

void MacroBodyCollector::applyPendingRepeat()
{
    // The sequence is in the body once already. The copies go through append(), so they take the room like
    // any other byte, and one too many refuses the definition, as in Windows Terminal's _applyPendingRepeat.
    auto const repeat = *std::exchange(_pendingRepeat, std::nullopt);
    auto const sequence = _body->substr(repeat.start);
    for ([[maybe_unused]] auto const _: std::views::iota(size_t { 1 }, std::max(repeat.count, size_t { 1 })))
        for (auto const byte: sequence)
        {
            append(byte);
            if (!_body)
                return;
        }
}

void MacroBodyCollector::append(char byte)
{
    if (_body->size() == _room)
    {
        refuse(MacroDefinitionError::TooLarge);
        return;
    }
    _body->push_back(byte);
}

void MacroBodyCollector::refuse(MacroDefinitionError error)
{
    _body = std::unexpected { error }; // none of it is kept, rather than a cut of it
}

void MacroBodyCollector::finalize()
{
    // DEC STD 070: "If a string terminator is encountered before the final semicolon, the repeat string is
    // terminated normally." A hex digit left without its pair is dropped.
    if (_body && _pendingRepeat)
        applyPendingRepeat();
    _done(std::move(_body));
}

} // namespace vtbackend
