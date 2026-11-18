// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphProtocol.hpp>

#include <vtbackend/vt/Base64Validation.hpp>

#include <core/Base64.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

namespace vtbackend
{

namespace
{
    using std::string_view;

    constexpr auto Whitespace = string_view { " \t\r\n" };

    /// The most codepoints a `grapheme=` may list.
    constexpr auto MaxGraphemeCodepoints = size_t { 32 };

    /// The payload formats this terminal takes, as the `s` reply lists them.
    constexpr auto SupportedFormats = string_view { "glyf" };

    [[nodiscard]] constexpr string_view trim(string_view text) noexcept
    {
        auto const first = text.find_first_not_of(Whitespace);
        if (first == string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(Whitespace) - first + 1);
    }

    /// One name of an enumeration, as it is spelled on the wire.
    template <typename Value>
    struct Named
    {
        string_view name;
        Value value;
    };

    constexpr auto SizeModes = std::array {
        Named { .name = "contain", .value = GlyphSizeMode::Contain },
        Named { .name = "height", .value = GlyphSizeMode::Height },
        Named { .name = "advance", .value = GlyphSizeMode::Advance },
        Named { .name = "cover", .value = GlyphSizeMode::Cover },
        Named { .name = "stretch", .value = GlyphSizeMode::Stretch },
    };

    constexpr auto HorizontalAlignments = std::array {
        Named { .name = "start", .value = GlyphHorizontalAlignment::Start },
        Named { .name = "center", .value = GlyphHorizontalAlignment::Center },
        Named { .name = "end", .value = GlyphHorizontalAlignment::End },
    };

    constexpr auto VerticalAlignments = std::array {
        Named { .name = "start", .value = GlyphVerticalAlignment::Start },
        Named { .name = "center", .value = GlyphVerticalAlignment::Center },
        Named { .name = "end", .value = GlyphVerticalAlignment::End },
        Named { .name = "baseline", .value = GlyphVerticalAlignment::Baseline },
    };

    constexpr auto RenderSpans = std::array {
        Named { .name = "1", .value = GlyphRenderSpan::OneCell },
        Named { .name = "2", .value = GlyphRenderSpan::TwoCells },
    };

    // An unknown `reply=` is the default, so it is looked up with value_or(All).
    constexpr auto ReplyModes = std::array {
        Named { .name = "0", .value = GlyphReplyMode::None },
        Named { .name = "1", .value = GlyphReplyMode::All },
        Named { .name = "2", .value = GlyphReplyMode::ErrorsOnly },
    };

    template <typename Value, size_t N>
    [[nodiscard]] constexpr std::optional<Value> named(std::array<Named<Value>, N> const& table,
                                                       string_view name) noexcept
    {
        auto const found = std::ranges::find(table, name, &Named<Value>::name);
        return found != table.end() ? std::optional { found->value } : std::nullopt;
    }

    /// The `key=value` parameters of a message. A key sent twice keeps its last value.
    class Parameters
    {
      public:
        explicit Parameters(string_view text)
        {
            for (auto const part: text | std::views::split(';'))
            {
                auto const parameter = trim(string_view { part.begin(), part.end() });
                auto const equals = parameter.find('=');
                if (equals == string_view::npos)
                    continue;
                _entries.emplace_back(trim(parameter.substr(0, equals)), trim(parameter.substr(equals + 1)));
            }
        }

        [[nodiscard]] std::optional<string_view> get(string_view key) const noexcept
        {
            auto const found = std::ranges::find(_entries | std::views::reverse, key, &Entry::first);
            return found != std::ranges::rend(_entries) ? std::optional { found->second } : std::nullopt;
        }

      private:
        using Entry = std::pair<string_view, string_view>;
        std::vector<Entry> _entries;
    };

    /// Reads a whole number, nothing before or after it.
    template <typename Number>
    [[nodiscard]] std::optional<Number> parseNumber(string_view text, int base = 10) noexcept
    {
        auto value = Number {};
        auto const* const end = text.data() + text.size();
        auto const [parsed, error] = std::from_chars(text.data(), end, value, base);
        return error == std::errc {} && parsed == end && !text.empty() ? std::optional { value }
                                                                       : std::nullopt;
    }

    /// Reads a hex codepoint: a Unicode scalar value, so no surrogate and nothing past U+10FFFF.
    [[nodiscard]] std::optional<char32_t> parseCodepoint(string_view text) noexcept
    {
        if (text.size() > 6)
            return std::nullopt;
        auto const value = parseNumber<uint32_t>(text, 16);
        if (!value || *value > 0x10FFFF || (*value >= 0xD800 && *value <= 0xDFFF))
            return std::nullopt;
        return static_cast<char32_t>(*value);
    }

    /// Reads a fraction between 0 and 1.
    [[nodiscard]] std::optional<float> parseFraction(string_view text) noexcept
    {
        // Digits and at most one point, checked by hand: std::from_chars for float is not in every standard
        // library this builds with, and the protocol's fractions are plain decimals.
        auto const point = text.find('.');
        auto const whole = text.substr(0, point);
        auto const decimals = point == string_view::npos ? string_view {} : text.substr(point + 1);
        auto const isDigit = [](char ch) {
            return ch >= '0' && ch <= '9';
        };
        if ((whole.empty() && decimals.empty()) || !std::ranges::all_of(whole, isDigit)
            || !std::ranges::all_of(decimals, isDigit) || whole.size() > 9)
            return std::nullopt;

        auto value = 0.0;
        for (auto const digit: whole)
            value = value * 10.0 + (digit - '0');
        auto scale = 0.1;
        for (auto const digit: decimals)
        {
            value += (digit - '0') * scale;
            scale *= 0.1;
        }
        return value <= 1.0 ? std::optional { static_cast<float>(value) } : std::nullopt;
    }

    /// Splits @p text at commas into exactly N trimmed parts.
    template <size_t N>
    [[nodiscard]] std::optional<std::array<string_view, N>> splitList(string_view text) noexcept
    {
        auto parts = std::array<string_view, N> {};
        auto count = size_t { 0 };
        for (auto const part: text | std::views::split(','))
        {
            if (count == N)
                return std::nullopt;
            parts[count++] = trim(string_view { part.begin(), part.end() });
        }
        return count == N ? std::optional { parts } : std::nullopt;
    }

    /// Reads an optional extent parameter, which is a positive number of units.
    /// @return The value, absent when the parameter is; no value at all when it is invalid.
    [[nodiscard]] std::optional<std::optional<uint16_t>> parseExtent(Parameters const& parameters,
                                                                     string_view key) noexcept
    {
        auto const text = parameters.get(key);
        if (!text)
            return std::optional<uint16_t> {};
        auto const value = parseNumber<uint16_t>(*text);
        if (!value || *value == 0)
            return std::nullopt;
        return std::optional<std::optional<uint16_t>> { std::in_place, value };
    }

    [[nodiscard]] std::optional<GlyphPadding> parsePadding(string_view text) noexcept
    {
        auto const parts = splitList<4>(text);
        if (!parts)
            return std::nullopt;
        auto const top = parseFraction((*parts)[0]);
        auto const right = parseFraction((*parts)[1]);
        auto const bottom = parseFraction((*parts)[2]);
        auto const left = parseFraction((*parts)[3]);
        if (!top || !right || !bottom || !left)
            return std::nullopt;
        // A pad that leaves no span is read as no padding at all.
        if (*left + *right >= 1.0F || *top + *bottom >= 1.0F)
            return GlyphPadding {};
        return GlyphPadding { .top = *top, .right = *right, .bottom = *bottom, .left = *left };
    }

    /// Reads the sizing and placement parameters of a registration.
    /// @return The layout, or nothing when any of them has an invalid value.
    [[nodiscard]] std::optional<GlyphLayout> parseLayout(Parameters const& parameters) noexcept
    {
        auto layout = GlyphLayout {};

        if (auto const text = parameters.get("upm"))
        {
            auto const unitsPerEm = parseNumber<uint16_t>(*text);
            if (!unitsPerEm || *unitsPerEm == 0)
                return std::nullopt;
            layout.unitsPerEm = *unitsPerEm;
        }

        auto const advanceWidth = parseExtent(parameters, "aw");
        auto const lineHeight = parseExtent(parameters, "lh");
        if (!advanceWidth || !lineHeight)
            return std::nullopt;
        layout.advanceWidth = *advanceWidth;
        layout.lineHeight = *lineHeight;

        if (auto const text = parameters.get("ymin"))
        {
            layout.extentBottom = parseNumber<int16_t>(*text);
            if (!layout.extentBottom)
                return std::nullopt;
        }

        if (auto const text = parameters.get("width"))
        {
            auto const span = named(RenderSpans, *text);
            if (!span)
                return std::nullopt;
            layout.span = *span;
        }

        if (auto const text = parameters.get("size"))
        {
            auto const size = named(SizeModes, *text);
            if (!size)
                return std::nullopt;
            layout.size = *size;
        }

        if (auto const text = parameters.get("align"))
        {
            auto const parts = splitList<2>(*text);
            auto const horizontal = parts ? named(HorizontalAlignments, (*parts)[0]) : std::nullopt;
            auto const vertical = parts ? named(VerticalAlignments, (*parts)[1]) : std::nullopt;
            if (!horizontal || !vertical)
                return std::nullopt;
            layout.horizontal = *horizontal;
            layout.vertical = *vertical;
        }

        if (auto const text = parameters.get("pad"))
        {
            auto const padding = parsePadding(*text);
            if (!padding)
                return std::nullopt;
            layout.padding = *padding;
        }

        return layout;
    }

    /// @return The `reply=` of a registration or a clear; the default when it is absent or names no mode.
    [[nodiscard]] GlyphReplyMode replyModeOf(Parameters const& parameters) noexcept
    {
        return parameters.get("reply")
            .and_then([](string_view text) { return named(ReplyModes, text); })
            .value_or(GlyphReplyMode::All);
    }

    using ParseResult = std::expected<GlyphCommand, GlyphCommandError>;

    [[nodiscard]] ParseResult parseSupport(string_view /*rest*/)
    {
        // `s` takes no parameters, and unknown ones are ignored.
        return GlyphSupportRequest {};
    }

    [[nodiscard]] ParseResult parseQuery(string_view rest)
    {
        auto const malformed = std::unexpected { GlyphCommandError { GlyphQueryMalformed {} } };
        auto const parameters = Parameters { rest };
        auto const codepoint = parameters.get("cp");
        auto const grapheme = parameters.get("grapheme");
        if (codepoint.has_value() == grapheme.has_value())
            return malformed;

        if (codepoint)
        {
            auto const value = parseCodepoint(*codepoint);
            if (!value)
                return malformed;
            return GlyphCodepointQuery { .codepoint = *value };
        }

        auto query = GlyphGraphemeQuery {};
        for (auto const part: *grapheme | std::views::split(','))
        {
            auto const value = parseCodepoint(trim(string_view { part.begin(), part.end() }));
            if (!value || query.codepoints.size() == MaxGraphemeCodepoints)
                return malformed;
            query.codepoints.push_back(*value);
        }
        // An empty list splits into no parts at all; a cluster holds at least one codepoint.
        if (query.codepoints.empty())
            return malformed;
        return query;
    }

    [[nodiscard]] ParseResult parseRegister(string_view rest)
    {
        // The payload is the last field, by its position and not by what it holds: base64 has no `;`, and
        // its padding is an `=`, so a field cannot be told from a parameter by its content.
        auto const lastSeparator = rest.rfind(';');
        auto const control =
            lastSeparator == string_view::npos ? string_view {} : rest.substr(0, lastSeparator);
        auto const payload = lastSeparator == string_view::npos ? rest : rest.substr(lastSeparator + 1);
        auto const parameters = Parameters { control };

        // Without a codepoint there is nothing for a reply to echo.
        auto const codepoint = parameters.get("cp").and_then(parseCodepoint);
        if (!codepoint)
            return std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } };

        // Read before anything that can fail, so every refusal below honours it.
        auto const reply = replyModeOf(parameters);
        auto const refused = [&](GlyphRegisterError reason) {
            return std::unexpected { GlyphCommandError {
                GlyphRegistrationRefused { .codepoint = *codepoint, .reason = reason, .reply = reply } } };
        };

        // The registration block is the protocol's security contract, so it is judged before the payload
        // is even looked at.
        if (!isGlyphRegistrationCodepoint(*codepoint))
            return refused(GlyphRegisterError::OutOfNamespace);

        if (parameters.get("fmt").value_or("glyf") != "glyf")
            return refused(GlyphRegisterError::MalformedPayload);

        auto const layout = parseLayout(parameters);
        if (!layout)
            return refused(GlyphRegisterError::MalformedPayload);

        auto const encoded = trim(payload);
        auto const digits = base64DigitCount(encoded);
        if (!digits || *digits == 0)
            return refused(GlyphRegisterError::MalformedPayload);
        // Judged from the digit count, before a byte is decoded.
        if (*digits / 4 * 3 > MaxGlyphPayloadBytes)
            return refused(GlyphRegisterError::PayloadTooLarge);

        auto record = std::vector<uint8_t>(core::base64::decodeLength(encoded));
        record.resize(core::base64::decode(encoded, record.data()));
        if (record.size() > MaxGlyphPayloadBytes)
            return refused(GlyphRegisterError::PayloadTooLarge);

        auto outline = decodeGlyf(record);
        if (!outline)
            return refused(outline.error());

        return GlyphRegistration {
            .codepoint = *codepoint, .reply = reply, .layout = *layout, .outline = std::move(*outline)
        };
    }

    [[nodiscard]] ParseResult parseClear(string_view rest)
    {
        auto const parameters = Parameters { rest };
        auto const reply = replyModeOf(parameters);
        auto const text = parameters.get("cp");
        if (!text)
            return GlyphClearRequest { .codepoint = std::nullopt, .reply = reply };
        auto const codepoint = parseCodepoint(*text);
        if (!codepoint)
            return std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } };
        if (!isGlyphRegistrationCodepoint(*codepoint))
            return std::unexpected { GlyphCommandError {
                GlyphClearRefused { .codepoint = *codepoint, .reply = reply } } };
        return GlyphClearRequest { .codepoint = *codepoint, .reply = reply };
    }

    /// One verb of the protocol and the reader of what follows it.
    struct Verb
    {
        string_view name;
        ParseResult (*parse)(string_view rest);
    };

    constexpr auto Verbs = std::array {
        Verb { .name = "s", .parse = parseSupport },
        Verb { .name = "q", .parse = parseQuery },
        Verb { .name = "r", .parse = parseRegister },
        Verb { .name = "c", .parse = parseClear },
    };

    [[nodiscard]] std::string hexList(std::u32string_view codepoints)
    {
        auto text = std::string {};
        for (auto const codepoint: codepoints)
            std::format_to(std::back_inserter(text),
                           "{}{:x}",
                           text.empty() ? "" : ",",
                           static_cast<uint32_t>(codepoint));
        return text;
    }

    [[nodiscard]] constexpr string_view statusOf(GlyphSources sources) noexcept
    {
        auto const system = sources.system == GlyphSources::System::Present;
        auto const glossary = sources.glossary == GlyphSources::Glossary::Present;
        if (system && glossary)
            return "system,glossary";
        if (system)
            return "system";
        return glossary ? "glossary" : "";
    }
} // namespace

std::expected<GlyphCommand, GlyphCommandError> parseGlyphCommand(std::string_view body)
{
    auto const ignored = std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } };
    if (!isGlyphProtocol(body))
        return ignored;

    auto const message = body.substr(body.find(';') + 1);
    auto const separator = message.find(';');
    auto const name = trim(message.substr(0, separator));
    auto const rest = separator == string_view::npos ? string_view {} : message.substr(separator + 1);

    auto const verb = std::ranges::find(Verbs, name, &Verb::name);
    return verb != Verbs.end() ? verb->parse(rest) : ignored;
}

std::string answerOversizedGlyphMessage(std::string_view front)
{
    if (!isGlyphProtocol(front))
        return {};
    auto const message = front.substr(front.find(';') + 1);
    auto const separator = message.find(';');
    // Only a registration carries a payload, so only one can be too large.
    if (separator == string_view::npos || trim(message.substr(0, separator)) != "r")
        return {};

    // What follows the last `;` is the payload that was cut off; the parameters are whole.
    auto const rest = message.substr(separator + 1);
    auto const parameters = Parameters { rest.substr(0, rest.rfind(';')) };
    auto const codepoint = parameters.get("cp").and_then(parseCodepoint);
    auto const reply = replyModeOf(parameters);
    if (!codepoint || reply == GlyphReplyMode::None)
        return {};
    // Out of the block it would have been refused for that, whatever its size.
    return glyph_reply::registrationRefused(*codepoint,
                                            isGlyphRegistrationCodepoint(*codepoint)
                                                ? GlyphRegisterError::PayloadTooLarge
                                                : GlyphRegisterError::OutOfNamespace);
}

namespace glyph_reply
{
    std::string support()
    {
        return std::format("\033_25a1;s;fmt={}\033\\", SupportedFormats);
    }

    std::string query(char32_t codepoint, GlyphSources sources)
    {
        return std::format(
            "\033_25a1;q;cp={:x};status={}\033\\", static_cast<uint32_t>(codepoint), statusOf(sources));
    }

    std::string graphemeUnsupported(std::u32string_view codepoints)
    {
        return std::format("\033_25a1;q;grapheme={};reason=unsupported\033\\", hexList(codepoints));
    }

    std::string queryMalformed()
    {
        return "\033_25a1;q;reason=malformed_query\033\\";
    }

    std::string registered(char32_t codepoint)
    {
        return std::format("\033_25a1;r;cp={:x};status=0\033\\", static_cast<uint32_t>(codepoint));
    }

    std::string registrationRefused(char32_t codepoint, GlyphRegisterError reason)
    {
        return std::format("\033_25a1;r;cp={:x};status=1;reason={}\033\\",
                           static_cast<uint32_t>(codepoint),
                           toString(reason));
    }

    std::string cleared(std::optional<char32_t> codepoint)
    {
        if (!codepoint)
            return "\033_25a1;c;status=0\033\\";
        return std::format("\033_25a1;c;cp={:x};status=0\033\\", static_cast<uint32_t>(*codepoint));
    }

    std::string clearRefused(char32_t codepoint)
    {
        return std::format("\033_25a1;c;cp={:x};status=1;reason=out_of_namespace\033\\",
                           static_cast<uint32_t>(codepoint));
    }
} // namespace glyph_reply

} // namespace vtbackend
