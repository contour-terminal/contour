// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SemanticPrompt.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    using namespace std::string_view_literals;

    /// Every `k=` value with a meaning here; any other reads as Initial.
    constexpr auto PromptKindTokens = std::array {
        std::pair { "i"sv, PromptKind::Initial },
        std::pair { "c"sv, PromptKind::Continuation },
        std::pair { "s"sv, PromptKind::Continuation },
        std::pair { "r"sv, PromptKind::Right },
    };

    /// Where kitty's shell lexer stands (kitty/launcher/shlex.h, next_word) on its way to the end of the
    /// first word.
    enum class Lexing : uint8_t
    {
        BeforeWord = 0,     ///< Blanks are skipped.
        Word,               ///< In the word, outside quotes.
        WordEscape,         ///< After a backslash outside quotes.
        SingleQuoted,       ///< Inside '...'.
        DoubleQuoted,       ///< Inside "...".
        DoubleQuotedEscape, ///< After a backslash inside "...".
        AnsiC,              ///< Inside $'...'.
        AnsiCEscape,        ///< After a backslash inside $'...'.
        AnsiCControl,       ///< After `\c` inside $'...'.
        AnsiCNumber,        ///< Among the digits of a numeric escape inside $'...'.
        Ended,              ///< At the blank after the word.
    };

    /// The base the digits of a numeric escape are in.
    enum class Radix : uint8_t
    {
        Octal = 8,
        Hexadecimal = 16,
    };

    /// A numeric escape of $'...' that a letter introduces.
    struct NumericEscape
    {
        char letter;        ///< The letter after the backslash.
        unsigned maxDigits; ///< How many hex digits it takes at most.
    };

    /// The letters of $'...' that escape a character other than themselves; any other stands for itself.
    constexpr auto AnsiCLetters = std::array {
        std::pair { 'a', '\a' },   std::pair { 'b', '\b' }, std::pair { 'e', '\x1B' },
        std::pair { 'E', '\x1B' }, std::pair { 'f', '\f' }, std::pair { 'n', '\n' },
        std::pair { 'r', '\r' },   std::pair { 't', '\t' }, std::pair { 'v', '\v' },
    };

    /// The numeric escapes of $'...' a letter introduces; an octal one starts with its first digit instead.
    constexpr auto AnsiCNumbers = std::array {
        NumericEscape { .letter = 'x', .maxDigits = 2 },
        NumericEscape { .letter = 'u', .maxDigits = 4 },
        NumericEscape { .letter = 'U', .maxDigits = 8 },
    };

    /// How many digits an octal escape takes at most, its first one included.
    constexpr auto MaxOctalDigits = 3U;

    /// The bits of the character after `\c` that the escape keeps.
    constexpr auto ControlBits = 0x1FU;

    [[nodiscard]] constexpr bool isBlank(char ch) noexcept
    {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    }

    /// @return The value of @p ch as a digit in @p radix, if it is one.
    [[nodiscard]] constexpr std::optional<unsigned> digitIn(Radix radix, char ch) noexcept
    {
        return radix == Radix::Octal ? core::digitValue<8>(ch) : core::digitValue<16>(ch);
    }

    /// kitty's shell lexer (kitty/launcher/shlex.h, next_word, with ANSI-C quoting), read one character at a
    /// time up to the end of the first word.
    class FirstWordLexer
    {
      public:
        /// Reads @p ch.
        /// @return Nothing, or why the text is malformed.
        [[nodiscard]] std::expected<void, ShellQuotingError> read(char ch)
        {
            switch (_state)
            {
                case Lexing::BeforeWord:
                    if (isBlank(ch))
                        return {};
                    return readUnquoted(ch);
                case Lexing::Word:
                    if (isBlank(ch))
                    {
                        _state = Lexing::Ended;
                        return {};
                    }
                    return readUnquoted(ch);
                case Lexing::WordEscape: return take(ch, Lexing::Word);
                case Lexing::SingleQuoted:
                    return ch == '\'' ? enter(Lexing::Word) : take(ch, Lexing::SingleQuoted);
                case Lexing::DoubleQuoted:
                    if (ch == '"')
                        return enter(Lexing::Word);
                    return ch == '\\' ? enter(Lexing::DoubleQuotedEscape) : take(ch, Lexing::DoubleQuoted);
                case Lexing::DoubleQuotedEscape: return take(ch, Lexing::DoubleQuoted);
                case Lexing::AnsiC: return readAnsiC(ch);
                case Lexing::AnsiCEscape: return readAnsiCEscape(ch);
                case Lexing::AnsiCControl:
                    return take(static_cast<char>(static_cast<unsigned char>(ch) & ControlBits),
                                Lexing::AnsiC);
                case Lexing::AnsiCNumber: return readAnsiCNumber(ch);
                case Lexing::Ended: return {};
            }
            core::unreachable();
        }

        /// @return Whether the first word has ended, so nothing more need be read.
        [[nodiscard]] bool hasEnded() const noexcept { return _state == Lexing::Ended; }

        /// Ends the text.
        /// @return The first word, empty when there was none, or why the text is malformed.
        [[nodiscard]] std::expected<std::string, ShellQuotingError> finish() &&
        {
            switch (_state)
            {
                case Lexing::BeforeWord:
                case Lexing::Word:
                case Lexing::Ended: return std::move(_word);
                case Lexing::WordEscape:
                case Lexing::DoubleQuotedEscape:
                case Lexing::AnsiCEscape: return std::unexpected(ShellQuotingError::TrailingBackslash);
                case Lexing::AnsiCControl: return std::unexpected(ShellQuotingError::IncompleteEscape);
                case Lexing::AnsiCNumber:
                    if (auto const ended = endNumber(); !ended)
                        return std::unexpected(ended.error());
                    return std::unexpected(ShellQuotingError::UnterminatedQuote);
                case Lexing::SingleQuoted:
                case Lexing::DoubleQuoted:
                case Lexing::AnsiC: return std::unexpected(ShellQuotingError::UnterminatedQuote);
            }
            core::unreachable();
        }

      private:
        /// Moves to @p state, writing nothing.
        [[nodiscard]] std::expected<void, ShellQuotingError> enter(Lexing state) noexcept
        {
            _state = state;
            _isAfterBareDollar = false;
            return {};
        }

        /// Writes @p ch as it is, and moves to @p state.
        [[nodiscard]] std::expected<void, ShellQuotingError> take(char ch, Lexing state)
        {
            _word += ch;
            return enter(state);
        }

        /// Reads @p ch in the word, outside quotes.
        [[nodiscard]] std::expected<void, ShellQuotingError> readUnquoted(char ch)
        {
            switch (ch)
            {
                case '"': return enter(Lexing::DoubleQuoted);
                case '\\': return enter(Lexing::WordEscape);
                case '\'':
                    // `$'` opens ANSI-C quoting, and the `$` is no part of the word -- but only one written
                    // as it is, unquoted and unescaped.
                    if (!_isAfterBareDollar)
                        return enter(Lexing::SingleQuoted);
                    _word.pop_back();
                    return enter(Lexing::AnsiC);
                default:
                    _word += ch;
                    _state = Lexing::Word;
                    _isAfterBareDollar = ch == '$';
                    return {};
            }
        }

        /// Reads @p ch inside $'...'.
        [[nodiscard]] std::expected<void, ShellQuotingError> readAnsiC(char ch)
        {
            if (ch == '\'')
                return enter(Lexing::Word);
            if (ch == '\\')
                return enter(Lexing::AnsiCEscape);
            return take(ch, Lexing::AnsiC);
        }

        /// Reads @p ch, the character after a backslash inside $'...'.
        [[nodiscard]] std::expected<void, ShellQuotingError> readAnsiCEscape(char ch)
        {
            if (auto const letter = std::ranges::find(AnsiCLetters, ch, &std::pair<char, char>::first);
                letter != AnsiCLetters.end())
                return take(letter->second, Lexing::AnsiC);
            if (ch == 'c')
                return enter(Lexing::AnsiCControl);
            if (auto const number = std::ranges::find(AnsiCNumbers, ch, &NumericEscape::letter);
                number != AnsiCNumbers.end())
                return startNumber(Radix::Hexadecimal, number->maxDigits, std::nullopt);
            if (auto const digit = digitIn(Radix::Octal, ch))
                return startNumber(Radix::Octal, MaxOctalDigits, digit);
            return take(ch, Lexing::AnsiC);
        }

        /// Starts a numeric escape in @p radix of at most @p maxDigits digits, @p firstDigit among them if
        /// given.
        [[nodiscard]] std::expected<void, ShellQuotingError> startNumber(Radix radix,
                                                                         unsigned maxDigits,
                                                                         std::optional<unsigned> firstDigit)
        {
            _number = Number { .radix = radix,
                               .value = firstDigit.value_or(0),
                               .digits = firstDigit ? 1U : 0U,
                               .maxDigits = maxDigits };
            return enter(Lexing::AnsiCNumber);
        }

        /// Reads @p ch among the digits of a numeric escape; one that is not a digit ends it, and is then
        /// read as what follows the escape.
        [[nodiscard]] std::expected<void, ShellQuotingError> readAnsiCNumber(char ch)
        {
            if (auto const digit = digitIn(_number.radix, ch))
            {
                _number.value = (_number.value * std::to_underlying(_number.radix)) + *digit;
                if (++_number.digits < _number.maxDigits)
                    return {};
                return endNumber();
            }
            if (auto const ended = endNumber(); !ended)
                return ended;
            return readAnsiC(ch);
        }

        /// Writes the code point the numeric escape read names, UTF-8 encoded.
        [[nodiscard]] std::expected<void, ShellQuotingError> endNumber()
        {
            static constexpr auto Surrogates = std::pair { 0xD800U, 0xDFFFU };
            static constexpr auto MaxCodepoint = 0x10FFFFU;

            if (_number.digits == 0)
                return std::unexpected(ShellQuotingError::IncompleteEscape);
            if (Surrogates.first <= _number.value && _number.value <= Surrogates.second)
                return std::unexpected(ShellQuotingError::NotACodepoint);
            if (_number.value <= MaxCodepoint) // kitty drops one past Unicode
                _word += unicode::convert_to<char>(static_cast<char32_t>(_number.value));
            return enter(Lexing::AnsiC);
        }

        /// A numeric escape being read.
        struct Number
        {
            Radix radix = Radix::Octal;
            unsigned value = 0;     ///< What its digits so far say.
            unsigned digits = 0;    ///< How many it has read.
            unsigned maxDigits = 0; ///< How many it takes at most.
        };

        std::string _word;
        Lexing _state = Lexing::BeforeWord;
        bool _isAfterBareDollar = false; ///< Whether the last character written was a `$` as it is.
        Number _number;
    };
} // namespace

PromptKind promptKindOf(std::string_view parameters) noexcept
{
    auto kind = PromptKind::Initial;
    core::forEachKeyValue(
        core::ForEachKeyValueParams { .text = parameters, .entryDelimiter = ';', .assignmentDelimiter = '=' },
        [&kind](std::string_view key, std::string_view value) noexcept {
            if (key != "k")
                return;
            auto const token =
                std::ranges::find(PromptKindTokens, value, &std::pair<std::string_view, PromptKind>::first);
            if (token != PromptKindTokens.end())
                kind = token->second;
        });
    return kind;
}

PromptRedraw promptRedrawOf(std::string_view parameters) noexcept
{
    auto redraw = PromptRedraw::OnResize;
    core::forEachKeyValue(
        core::ForEachKeyValueParams { .text = parameters, .entryDelimiter = ';', .assignmentDelimiter = '=' },
        [&redraw](std::string_view key, std::string_view value) noexcept {
            if (key == "redraw" && value == "0")
                redraw = PromptRedraw::Never;
        });
    return redraw;
}

std::expected<std::string, ShellQuotingError> firstShellWord(std::string_view text)
{
    auto lexer = FirstWordLexer {};
    for (auto const ch: text)
    {
        if (auto const read = lexer.read(ch); !read)
            return std::unexpected(read.error());
        if (lexer.hasEnded())
            break;
    }
    return std::move(lexer).finish();
}

std::optional<std::string> reportedCommandLine(std::string_view parameters)
{
    constexpr auto UrlKey = "cmdline_url="sv;
    constexpr auto QuotedKey = "cmdline="sv;

    // Not core::forEachKeyValue: either key takes the rest of the sequence, as kitty's decode_cmdline does.
    for (auto const entry: std::views::split(parameters, ';'))
    {
        auto const rest = std::string_view { entry.begin(), parameters.end() };
        if (rest.starts_with(UrlKey))
        {
            if (auto line = core::unescapeURL(rest.substr(UrlKey.size())); !line.empty())
                return line;
            return std::nullopt;
        }
        if (rest.starts_with(QuotedKey))
        {
            auto line = firstShellWord(rest.substr(QuotedKey.size()));
            if (line && !line->empty())
                return std::move(*line);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace vtbackend
