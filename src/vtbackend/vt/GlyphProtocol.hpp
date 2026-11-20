// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/VectorGlyph.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

/// The Glyph Protocol's wire format: what an `APC 25a1 ; ...` body says, and how its replies are phrased.
///
/// Specification: https://github.com/raphamorim/rio/blob/main/specs/glyph-protocol.md (v1.17). Every
/// payload format is optional there. This terminal takes `vg`, the recommended one (@see VectorGlyph.hpp),
/// and refuses the font-table formats (`glyf`, `colrv0`, `colrv1`) as formats it does not list.
namespace vtbackend
{

/// The first codepoint a glyph may be registered at: the start of Supplementary Private Use Area-B.
inline constexpr char32_t FirstGlyphRegistrationCodepoint = 0x100000;

/// How many codepoints the registration block holds, and so how many glyphs a session can register.
inline constexpr size_t GlyphRegistrationSlots = 1024;

/// The largest payload a registration may carry, in bytes.
inline constexpr size_t MaxGlyphPayloadBytes = size_t { 32 } * 1024;

/// @param codepoint Any codepoint.
/// @return Whether a glyph may be registered at @p codepoint.
[[nodiscard]] constexpr bool isGlyphRegistrationCodepoint(char32_t codepoint) noexcept
{
    return codepoint >= FirstGlyphRegistrationCodepoint
           && codepoint - FirstGlyphRegistrationCodepoint < GlyphRegistrationSlots;
}

/// What every Glyph Protocol message begins with: U+25A1 in hex, and the separator.
inline constexpr std::string_view GlyphProtocolIdentifier = "25a1;";

/// @param body An APC body, without its introducer or terminator.
/// @return Whether @p body is a Glyph Protocol message.
[[nodiscard]] constexpr bool isGlyphProtocol(std::string_view body) noexcept
{
    return body.starts_with(GlyphProtocolIdentifier);
}

/// Why a registration is refused; each is one `reason=` of the `r` reply.
enum class GlyphRegisterError : uint8_t
{
    OutOfNamespace,    ///< The codepoint is outside the registration block.
    UnsupportedFormat, ///< `fmt` names a format this terminal does not list in its `s` reply.
    MalformedPayload,  ///< The payload is not what its format allows, or a placement key is invalid.
    PayloadTooLarge,   ///< The payload is over MaxGlyphPayloadBytes.
    OutlineTooLarge,   ///< The payload is past one of its format's limits.
};

/// @return The `reason=` value that names @p error on the wire.
[[nodiscard]] std::string_view toString(GlyphRegisterError error) noexcept;

/// How a glyph's viewBox is scaled into its render span (`fit=`), as SVG's `preserveAspectRatio` scales a
/// viewBox into a viewport.
enum class GlyphFit : uint8_t
{
    Contain, ///< The whole viewBox fits the span on both axes.
    Cover,   ///< The viewBox covers the span on both axes.
    Stretch, ///< Each axis fills the span independently.
};

/// Where the scaled viewBox sits in the span on one axis (`align=`). Horizontally the start is the left;
/// vertically it is the top, as SVG's `xMin` and `yMin`.
enum class GlyphAlignment : uint8_t
{
    Center,
    Start,
    End,
};

/// How many cells of pixels a glyph is drawn across (`width=`). The codepoint's own width stays one.
enum class GlyphRenderSpan : uint8_t
{
    OneCell = 1,
    TwoCells = 2,
};

/// How a registered glyph is sized and placed in its cell.
struct GlyphLayout
{
    GlyphFit fit = GlyphFit::Contain;
    GlyphAlignment horizontal = GlyphAlignment::Center;
    GlyphAlignment vertical = GlyphAlignment::Center;
    GlyphRenderSpan span = GlyphRenderSpan::OneCell;

    bool operator==(GlyphLayout const&) const = default;
};

/// Which replies a registration or a clear asks for (`reply=`).
enum class GlyphReplyMode : uint8_t
{
    All,        ///< Success and failure (`reply=1`, the default).
    None,       ///< Neither (`reply=0`).
    ErrorsOnly, ///< Failure only (`reply=2`).
};

/// `s`: which payload formats does the terminal take?
struct GlyphSupportRequest
{
    bool operator==(GlyphSupportRequest const&) const = default;
};

/// `q ; cp=`: what draws this codepoint?
struct GlyphCodepointQuery
{
    char32_t codepoint = 0;

    bool operator==(GlyphCodepointQuery const&) const = default;
};

/// `q ; grapheme=`: what draws this cluster?
struct GlyphGraphemeQuery
{
    std::u32string codepoints;

    bool operator==(GlyphGraphemeQuery const&) const = default;
};

/// The keys of a registration that state a glyph, as they were sent: everything but where it goes.
struct GlyphStatement
{
    std::optional<std::string_view> viewBox; ///< `vb=`; required.
    std::optional<std::string_view> fit;     ///< `fit=`.
    std::optional<std::string_view> align;   ///< `align=`.
    std::optional<std::string_view> width;   ///< `width=`.
    std::string_view payload;                ///< The `vg` payload.
};

/// A glyph as an application stated it, judged: nothing in it is unread.
struct GlyphDefinition
{
    GlyphLayout layout {};
    VectorGlyph glyph {};
    /// The payload as it was sent, which is how a glyph travels on (to a daemon's clients): whoever
    /// receives it reads it with this same parser, and so judges it by the same rules.
    std::string payload;

    bool operator==(GlyphDefinition const&) const = default;
};

/// Reads a glyph from the keys that state it.
/// @param statement The keys and the payload.
/// @return The glyph, or why it is refused.
[[nodiscard]] std::expected<GlyphDefinition, GlyphRegisterError> parseGlyphDefinition(
    GlyphStatement const& statement);

/// `r`: register a glyph at a codepoint of the registration block.
struct GlyphRegistration
{
    char32_t codepoint = 0;
    GlyphReplyMode reply = GlyphReplyMode::All;
    GlyphDefinition definition {};

    bool operator==(GlyphRegistration const&) const = default;
};

/// `c`: clear one registration, or all of them.
struct GlyphClearRequest
{
    std::optional<char32_t> codepoint; ///< The one to clear; absent clears every registration.
    GlyphReplyMode reply = GlyphReplyMode::All;

    bool operator==(GlyphClearRequest const&) const = default;
};

/// One Glyph Protocol request.
using GlyphCommand = std::variant<GlyphSupportRequest,
                                  GlyphCodepointQuery,
                                  GlyphGraphemeQuery,
                                  GlyphRegistration,
                                  GlyphClearRequest>;

/// A message nothing can be answered to: an unknown verb, or a request without the codepoint a reply
/// would have to echo.
struct GlyphMessageIgnored
{
    bool operator==(GlyphMessageIgnored const&) const = default;
};

/// A registration that is refused.
struct GlyphRegistrationRefused
{
    char32_t codepoint = 0;
    GlyphRegisterError reason = GlyphRegisterError::MalformedPayload;
    GlyphReplyMode reply = GlyphReplyMode::All;

    bool operator==(GlyphRegistrationRefused const&) const = default;
};

/// A clear naming a codepoint outside the registration block.
struct GlyphClearRefused
{
    char32_t codepoint = 0;
    GlyphReplyMode reply = GlyphReplyMode::All;

    bool operator==(GlyphClearRefused const&) const = default;
};

/// A query with neither or both of `cp` and `grapheme`, or with a value that is not a scalar value.
struct GlyphQueryMalformed
{
    bool operator==(GlyphQueryMalformed const&) const = default;
};

/// Why a message is not a request.
using GlyphCommandError =
    std::variant<GlyphMessageIgnored, GlyphRegistrationRefused, GlyphClearRefused, GlyphQueryMalformed>;

/// Reads one Glyph Protocol message.
/// @param body The APC body, starting with `25a1;` (@see isGlyphProtocol).
/// @return The request, or why there is none.
[[nodiscard]] std::expected<GlyphCommand, GlyphCommandError> parseGlyphCommand(std::string_view body);

/// Answers a message that was cut off for being longer than an APC body may be.
/// @param front The start of the message, as much of it as arrived: the verb and the parameters, which
///              precede the payload.
/// @return The refusal of a registration, `reason=payload_too_large`, as a complete APC sequence; empty when
///         @p front is not a Glyph Protocol registration, names no codepoint, or asked for no reply.
[[nodiscard]] std::string answerOversizedGlyphMessage(std::string_view front);

/// What draws a codepoint, as a `q` reply's `status=` lists it.
struct GlyphSources
{
    /// Whether a font in the fallback chain draws it.
    enum class System : uint8_t
    {
        Absent,
        Present,
    };

    /// Whether the session has a glyph registered for it.
    enum class Glossary : uint8_t
    {
        Absent,
        Present,
    };

    System system = System::Absent;
    Glossary glossary = Glossary::Absent;

    bool operator==(GlyphSources const&) const = default;
};

/// The replies, each a complete APC sequence.
namespace glyph_reply
{
    /// @return The `s` reply: the payload formats this terminal takes.
    [[nodiscard]] std::string support();

    /// @param codepoint The codepoint asked about.
    /// @param sources What draws it.
    /// @return The `q ; cp=` reply.
    [[nodiscard]] std::string query(char32_t codepoint, GlyphSources sources);

    /// @param codepoints The cluster asked about.
    /// @return The reply to a `q ; grapheme=` this terminal does not answer.
    [[nodiscard]] std::string graphemeUnsupported(std::u32string_view codepoints);

    /// @return The reply to a query that cannot be read.
    [[nodiscard]] std::string queryMalformed();

    /// @param codepoint The codepoint registered.
    /// @return The reply to a registration that was taken.
    [[nodiscard]] std::string registered(char32_t codepoint);

    /// @param codepoint The codepoint the registration named.
    /// @param reason Why it is refused.
    /// @return The reply to a registration that was refused.
    [[nodiscard]] std::string registrationRefused(char32_t codepoint, GlyphRegisterError reason);

    /// @param codepoint The codepoint cleared; absent when every registration was.
    /// @return The reply to a clear that was carried out.
    [[nodiscard]] std::string cleared(std::optional<char32_t> codepoint);

    /// @param codepoint The codepoint the clear named.
    /// @return The reply to a clear naming a codepoint outside the registration block.
    [[nodiscard]] std::string clearRefused(char32_t codepoint);
} // namespace glyph_reply

} // namespace vtbackend
