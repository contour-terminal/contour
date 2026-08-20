// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/HierarchicalContext.hpp>

#include <crispy/Utils.hpp>

#include <algorithm>
#include <optional>

using std::string;
using std::string_view;

namespace vtbackend
{

namespace
{
    /// Whether @p ch is a byte SAFE admits directly, i.e. without an escape.
    ///
    /// `SAFE = %x20-3a / %x3c-5b / %x5d-7e / ESCSEMICOLON / ESCBACKSLASH` -- so `;` (0x3b) and `\`
    /// (0x5c) are excluded, being spelled by the two escapes instead. Bytes above 0x7e are admitted
    /// here although the ABNF's own production does not name them: the specification's prose overrides
    /// it, allowing "the ASCII byte range > 0x7f" so that UTF-8 values survive. 0x7f itself is a
    /// control character and is not allowed -- and it is reachable, because vtparser's OSC range ends
    /// at 0x7f rather than 0x7e.
    [[nodiscard]] constexpr bool isSafeByte(unsigned char ch) noexcept
    {
        if (ch < 0x20 || ch == 0x7f)
            return false;
        return ch != ';' && ch != '\\';
    }

    /// Whether @p ch is the hex digit @p lower, in either case.
    [[nodiscard]] constexpr bool matchesHexDigit(char ch, char lower) noexcept
    {
        return ch == lower || ch == static_cast<char>(lower - ('a' - 'A'));
    }

    /// The byte the four characters at @p raw[index] escape, if they are one of the two escapes.
    ///
    /// ABNF quoted strings are case-insensitive (RFC 5234 section 2.3), so `\x3b`, `\x3B`, `\X3b` and
    /// `\X3B` all spell ESCSEMICOLON -- and likewise for ESCBACKSLASH.
    [[nodiscard]] constexpr std::optional<char> escapedByteAt(string_view raw, size_t index) noexcept
    {
        if (index + 4 > raw.size())
            return std::nullopt;
        if (!matchesHexDigit(raw[index + 1], 'x'))
            return std::nullopt;
        if (raw[index + 2] == '3' && matchesHexDigit(raw[index + 3], 'b'))
            return ';';
        if (raw[index + 2] == '5' && matchesHexDigit(raw[index + 3], 'c'))
            return '\\';
        return std::nullopt;
    }

    /// One key=value entry of the payload, still escaped.
    struct RawField
    {
        string_view key;
        string_view value;
    };

    /// Splits @p entry at its first `=`.
    /// @return The field, or nullopt when @p entry carries no `=` at all.
    [[nodiscard]] constexpr std::optional<RawField> splitField(string_view entry) noexcept
    {
        auto const separator = entry.find('=');
        if (separator == string_view::npos)
            return std::nullopt;
        return RawField { .key = entry.substr(0, separator), .value = entry.substr(separator + 1) };
    }

    /// Whether @p value is `ID128 = 32*36(HEX / "-")`.
    [[nodiscard]] constexpr bool isId128(string_view value) noexcept
    {
        if (value.size() < 32 || value.size() > 36)
            return false;
        return std::ranges::all_of(value, [](char ch) {
            return ch == '-' || (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')
                   || (ch >= 'A' && ch <= 'F');
        });
    }

    /// Parses `UINT64 = 1*20DECIMAL`.
    ///
    /// The length bound is the GRAMMAR's, not an overflow guard: crispy::toInteger range-checks every
    /// digit against the target type and returns nullopt on overflow, so a 21-digit run would be
    /// rejected anyway. Stating the ABNF here keeps `1*20DECIMAL` enforced where it is written.
    [[nodiscard]] constexpr std::optional<uint64_t> parseUint64(string_view value) noexcept
    {
        if (value.empty() || value.size() > 20)
            return std::nullopt;
        if (!std::ranges::all_of(value, [](char ch) { return ch >= '0' && ch <= '9'; }))
            return std::nullopt;
        return crispy::toInteger<10, uint64_t>(value);
    }

    /// Appends @p raw's decoded bytes to @p scratch and returns a view of them.
    ///
    /// The view is into @p scratch, which the caller reserved up front so it never reallocates and the
    /// views handed back never dangle.
    /// @return The decoded value, or nullopt when @p raw is not a valid run of SAFE elements.
    [[nodiscard]] std::optional<string_view> decodeInto(string& scratch, string_view raw)
    {
        auto const begin = scratch.size();
        if (!unescapeContextValue(raw, scratch))
        {
            scratch.resize(begin);
            return std::nullopt;
        }
        return string_view { scratch }.substr(begin, scratch.size() - begin);
    }

    /// Records one decoded STARTFIELD on @p command, or drops it when it fails its validator.
    void applyStartField(ContextCommand& command, string_view key, string_view value)
    {
        // Generated from VTBACKEND_CONTEXT_FIELDS so a sixteenth field is one row there, not an edit
        // here. Each arm validates before recording: a field that fails is simply absent, and the
        // present-bit is what later tells "sent empty" apart from "never mentioned".
#define VTBACKEND_CONTEXT_FIELD_APPLY(Name, Bit, Spelling, Member, Kind, Max) \
    if (key == (Spelling))                                                    \
    {                                                                         \
        applyField##Kind(command, ContextField::Name, command.Member, value); \
        return;                                                               \
    }

        // The five validators, as local lambdas so the table above reads as a table.
        auto const applyFieldText =
            [](ContextCommand& cmd, ContextField field, string_view& member, string_view value) {
                // `1*255SAFE`: empty is not a value, and an over-long one is DISCARDED rather than
                // truncated -- a truncated path names a real directory that is not the one meant, and
                // something will eventually open it. An absent field is honestly absent.
                if (value.empty() || value.size() > MaxContextFieldLength)
                    return;
                member = value;
                cmd.present.enable(field);
            };
        auto const applyFieldOptionalText =
            [](ContextCommand& cmd, ContextField field, string_view& member, string_view value) {
                // `*255SAFE`: cmdline= alone may legitimately be empty, so honour that difference.
                if (value.size() > MaxContextFieldLength)
                    return;
                member = value;
                cmd.present.enable(field);
            };
        auto const applyFieldId128 =
            [](ContextCommand& cmd, ContextField field, string_view& member, string_view value) {
                if (!isId128(value))
                    return;
                member = value;
                cmd.present.enable(field);
            };
        auto const applyFieldUint64 =
            [](ContextCommand& cmd, ContextField field, uint64_t& member, string_view value) {
                auto const parsed = parseUint64(value);
                if (!parsed)
                    return;
                member = *parsed;
                cmd.present.enable(field);
            };
        auto const applyFieldEnum =
            [](ContextCommand& cmd, ContextField field, ContextType& member, string_view value) {
                auto const type = contextTypeFrom(value);
                if (type == ContextType::None)
                    return; // an unknown type is an unknown field: ignored, rest of the sequence stands
                member = type;
                cmd.present.enable(field);
            };

        VTBACKEND_CONTEXT_FIELDS(VTBACKEND_CONTEXT_FIELD_APPLY)
#undef VTBACKEND_CONTEXT_FIELD_APPLY
    }

    /// Records one decoded ENDFIELD on @p command's outcome, or drops it.
    void applyEndField(ContextCommand& command, string_view key, string_view value)
    {
        if (key == "exit")
        {
            if (auto const exit = contextExitFrom(value); exit != ContextExit::Unknown)
                command.outcome.exit = exit;
            return;
        }
        if (key == "status")
        {
            if (auto const status = parseUint64(value))
                command.outcome.status = *status;
            return;
        }
        if (key == "signal")
        {
            if (auto const signal = contextSignalFrom(value); signal != ContextSignal::None)
                command.outcome.signal = signal;
            return;
        }
        // Anything else is an unknown field: ignored, and the rest of the sequence still stands.
    }
} // namespace

bool unescapeContextValue(string_view raw, string& out)
{
    for (auto index = size_t {}; index < raw.size();)
    {
        auto const ch = raw[index];
        if (ch == '\\')
        {
            auto const escaped = escapedByteAt(raw, index);
            if (!escaped)
                return false; // a bare 0x5c, which SAFE does not admit in any position
            out.push_back(*escaped);
            index += 4;
            continue;
        }
        if (!isSafeByte(static_cast<unsigned char>(ch)))
            return false;
        out.push_back(ch);
        ++index;
    }
    return true;
}

std::expected<ContextCommand, ContextParseError> parseContextSequence(string_view payload, string& scratch)
{
    if (payload.size() > MaxContextPayloadLength)
        return std::unexpected(ContextParseError::PayloadTooLong);

    auto command = ContextCommand {};
    if (payload.starts_with("start="))
    {
        command.verb = ContextVerb::Start;
        payload.remove_prefix(6);
    }
    else if (payload.starts_with("end="))
    {
        command.verb = ContextVerb::End;
        payload.remove_prefix(4);
    }
    else
        return std::unexpected(ContextParseError::MissingVerb);

    // Reserved once, to what any decoding of this payload can need: an escape only ever shrinks, so
    // the decoded total can never exceed the raw total. That is what lets every view handed back point
    // into this buffer safely.
    scratch.clear();
    scratch.reserve(payload.size());

    auto const firstSeparator = payload.find(';');
    auto const rawIdentifier = payload.substr(0, firstSeparator);
    if (rawIdentifier.empty())
        return std::unexpected(ContextParseError::EmptyIdentifier);

    auto const identifier = decodeInto(scratch, rawIdentifier);
    if (!identifier)
        return std::unexpected(ContextParseError::MalformedIdentifier);
    // Measured on the DECODED value, because SAFE counts elements and ESCSEMICOLON is ONE of them
    // spelled with four characters. Validating the raw length instead would reject a legal id.
    if (identifier->size() > MaxContextIdentifierLength)
        return std::unexpected(ContextParseError::IdentifierTooLong);
    command.identifier = *identifier;

    if (firstSeparator == string_view::npos)
        return command; // a bare `start=ID`, which the ABNF admits: `*(";" STARTFIELD)`

    // The field order is undefined -- `type=` may appear last, or in the middle -- so each entry is
    // dispatched independently against the field table and nothing is read until the payload is done.
    crispy::split(payload.substr(firstSeparator + 1), ';', [&](string_view entry) {
        auto const field = splitField(entry);
        if (!field)
            return true; // a bare token with no `=` is not a field; ignore it and keep going

        auto const value = decodeInto(scratch, field->value);
        if (!value)
            return true; // an invalid field is dropped; the rest of the sequence still stands

        if (command.verb == ContextVerb::Start)
            applyStartField(command, field->key, *value);
        else
            applyEndField(command, field->key, *value);
        return true;
    });

    return command;
}

} // namespace vtbackend
