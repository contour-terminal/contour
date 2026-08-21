// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/TerminalContext.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace vtbackend
{

/// The longest CTXID the ABNF admits: `CTXID = 1*64SAFE`, counted in DECODED bytes.
inline constexpr size_t MaxContextIdentifierLength = 64;

/// The longest decoded value any metadata field may carry: every text field is `*255SAFE`.
inline constexpr size_t MaxContextFieldLength = 255;

/// The longest payload the decoder will look at.
///
/// Derived, not guessed: the ABNF's largest legal payload is one 64-byte CTXID plus fifteen fields of
/// at most 255 decoded bytes, and a decoded byte costs at most four raw characters (`\x5c`), so no
/// conforming sequence exceeds ~16 kB. Being under Sequence::MaxOscLength (50 kB) is the point: a
/// legal sequence can therefore never have been truncated by the time we see it, so the decoder never
/// has to guess whether a short last field was sent short or cut off.
inline constexpr size_t MaxContextPayloadLength = 16384;

/// Why an OSC 3008 payload could not be decoded at all.
///
/// Only failures of the SEQUENCE are errors. A malformed FIELD is not: the protocol is lenient, so an
/// invalid or unknown field is dropped and the rest of the sequence stands. What cannot be recovered
/// from is a broken identifier -- the subject of the sequence, not one of its fields.
enum class ContextParseError : uint8_t
{
    MissingVerb = 0,     ///< The payload begins with neither `start=` nor `end=`.
    EmptyIdentifier,     ///< The CTXID was empty; the ABNF requires at least one SAFE.
    IdentifierTooLong,   ///< The CTXID exceeded @ref MaxContextIdentifierLength decoded bytes.
    MalformedIdentifier, ///< The CTXID held a byte SAFE does not admit, or a broken escape.
    PayloadTooLong,      ///< The payload exceeded @ref MaxContextPayloadLength.
};

/// Names a decode failure for diagnostics.
///
/// A switch rather than a table, so that -Wswitch makes adding an enumerator without its name a build
/// break -- the same reason proto::toString(DecodeError) is one.
[[nodiscard]] constexpr std::string_view toString(ContextParseError error) noexcept
{
    switch (error)
    {
        case ContextParseError::MissingVerb: return "MissingVerb";
        case ContextParseError::EmptyIdentifier: return "EmptyIdentifier";
        case ContextParseError::IdentifierTooLong: return "IdentifierTooLong";
        case ContextParseError::MalformedIdentifier: return "MalformedIdentifier";
        case ContextParseError::PayloadTooLong: return "PayloadTooLong";
    }
    return "Unknown";
}

/// Resolves the two escapes the ABNF defines.
///
/// `ESCSEMICOLON` and `ESCBACKSLASH` are the LITERAL four-character sequences `\x3b` and `\x5c` -- not
/// C escapes, and not a general `\xNN` scheme: no other hex pair is an escape, and a bare backslash is
/// not a SAFE element at all (SAFE resumes at %x5d, one past it). ABNF quoted strings are
/// case-insensitive per RFC 5234 section 2.3, so all four spellings of each are accepted.
///
/// Anything else carrying a backslash is therefore a value the grammar does not admit, and the caller
/// drops that FIELD while keeping the rest of the sequence -- which is what the specification means by
/// processing "in a lenient, graceful fashion".
///
/// Bytes above 0x7f pass through verbatim: the specification deliberately departs from ECMA-48 to
/// allow them. Control bytes do not -- and 0x7f in particular is reachable, because the parser's OSC
/// range ends at it.
///
/// @param raw The field's text as it arrived.
/// @param out Receives the decoded bytes, APPENDED. The caller reserves once and reuses.
/// @return Whether @p raw was a valid sequence of SAFE elements. On false @p out is left with whatever
///         was appended before the fault; the caller drops the field.
[[nodiscard]] bool unescapeContextValue(std::string_view raw, std::string& out);

/// Decodes one OSC 3008 payload.
///
/// @param payload The OSC body with the code and its following `;` already removed, i.e.
///        `start=ID;type=shell` -- @see SequenceBuilder::dispatchOSC, which erases exactly that.
/// @param scratch A buffer the decoder owns for the duration of the call. Cleared and reserved on
///        entry to what any decoding of @p payload can need (an escape only ever SHRINKS), so it never
///        reallocates mid-decode and the returned views never dangle. Injected rather than static: a
///        hidden buffer is shared mutable state, and a member of the caller costs one allocation per
///        session rather than one per prompt.
/// @return The decoded command, whose views point into @p payload and @p scratch, or why the sequence
///         as a whole was rejected.
[[nodiscard]] std::expected<ContextCommand, ContextParseError> parseContextSequence(std::string_view payload,
                                                                                    std::string& scratch);

} // namespace vtbackend
