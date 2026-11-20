// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphProtocol.hpp>

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
    constexpr auto SupportedFormats = string_view { "vg" };

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

    constexpr auto Fits = std::array {
        Named { .name = "contain", .value = GlyphFit::Contain },
        Named { .name = "cover", .value = GlyphFit::Cover },
        Named { .name = "stretch", .value = GlyphFit::Stretch },
    };

    // A viewBox has no baseline, so there is no such alignment.
    constexpr auto Alignments = std::array {
        Named { .name = "start", .value = GlyphAlignment::Start },
        Named { .name = "center", .value = GlyphAlignment::Center },
        Named { .name = "end", .value = GlyphAlignment::End },
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
        // The payload is the last field, by its position and not by what it holds: a `vg` payload has no
        // `;`, and it does hold `=`, so a field cannot be told from a parameter by its content.
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

        // A registration without `fmt` is `glyf`, as it was before formats were negotiated; that and every
        // other format this terminal does not list has a reason of its own, so that a client can tell a
        // format it cannot use here from a payload that is wrong.
        if (parameters.get("fmt").value_or("glyf") != SupportedFormats)
            return refused(GlyphRegisterError::UnsupportedFormat);

        auto definition = parseGlyphDefinition(GlyphStatement { .viewBox = parameters.get("vb"),
                                                                .fit = parameters.get("fit"),
                                                                .align = parameters.get("align"),
                                                                .width = parameters.get("width"),
                                                                .payload = payload });
        if (!definition)
            return refused(definition.error());
        return GlyphRegistration { .codepoint = *codepoint,
                                   .reply = reply,
                                   .definition = std::move(*definition) };
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

std::string_view toString(GlyphRegisterError error) noexcept
{
    switch (error)
    {
        case GlyphRegisterError::OutOfNamespace: return "out_of_namespace";
        case GlyphRegisterError::UnsupportedFormat: return "unsupported_format";
        case GlyphRegisterError::MalformedPayload: return "malformed_payload";
        case GlyphRegisterError::PayloadTooLarge: return "payload_too_large";
        case GlyphRegisterError::OutlineTooLarge: return "outline_too_large";
    }
    std::unreachable();
}

std::expected<GlyphDefinition, GlyphRegisterError> parseGlyphDefinition(GlyphStatement const& statement)
{
    auto const malformed = std::unexpected { GlyphRegisterError::MalformedPayload };
    auto layout = GlyphLayout {};

    if (statement.width)
    {
        auto const span = named(RenderSpans, *statement.width);
        if (!span)
            return malformed;
        layout.span = *span;
    }
    if (statement.fit)
    {
        auto const fit = named(Fits, *statement.fit);
        if (!fit)
            return malformed;
        layout.fit = *fit;
    }
    if (statement.align)
    {
        auto const parts = splitList<2>(*statement.align);
        auto const horizontal = parts ? named(Alignments, (*parts)[0]) : std::nullopt;
        auto const vertical = parts ? named(Alignments, (*parts)[1]) : std::nullopt;
        if (!horizontal || !vertical)
            return malformed;
        layout.horizontal = *horizontal;
        layout.vertical = *vertical;
    }

    auto const payload = trim(statement.payload);
    if (payload.size() > MaxGlyphPayloadBytes)
        return std::unexpected { GlyphRegisterError::PayloadTooLarge };
    auto const viewBox = statement.viewBox.and_then(parseVectorViewBox);
    if (!viewBox)
        return malformed;
    return parseVectorGlyph(payload, *viewBox)
        .transform([&](VectorGlyph glyph) {
            return GlyphDefinition { .layout = layout,
                                     .glyph = std::move(glyph),
                                     .payload = std::string { payload } };
        })
        .transform_error([](VectorGlyphError error) {
            return error == VectorGlyphError::TooLarge ? GlyphRegisterError::OutlineTooLarge
                                                       : GlyphRegisterError::MalformedPayload;
        });
}

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
