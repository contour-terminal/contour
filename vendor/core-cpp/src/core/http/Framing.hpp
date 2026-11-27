// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How an HTTP/1.1 message is delimited on the wire (RFC 9112): the limits every parser is bound
/// by, the body-framing rules of section 6.3, the incremental head parser, and the chunked codec of
/// section 7. Nothing here touches a socket; every input is bytes the caller already holds.

#include <core/http/HttpExport.hpp>
#include <core/http/Message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace core::http
{

/// The bounds a peer's message is held to, fixed when a parser or connection is constructed.
///
/// A peer is untrusted, so nothing it sends makes the host hold more than these: every parser
/// refuses past them instead of buffering on.
struct HttpLimits
{
    /// The start line and the fields, line endings included.
    std::size_t maxHeadBytes = std::size_t { 64 } * 1024;
    /// Any one line of a head or a trailer section, the start line included.
    std::size_t maxFieldBytes = std::size_t { 16 } * 1024;
    /// The most field lines in a head, and the most in a trailer section; each is a host object.
    std::size_t maxFields = 100;
    /// A chunked body's trailer section.
    std::size_t maxTrailerBytes = std::size_t { 16 } * 1024;
    /// A chunk's size line, its extensions included.
    std::size_t maxChunkLineBytes = 1024;
    /// The most body bytes handed out at once.
    std::size_t maxReadChunk = std::size_t { 64 } * 1024;
    /// The largest body a chunked decoder accepts; empty for no limit.
    std::optional<std::uint64_t> maxBodyBytes;
};

/// Which kind of message a parser reads, which selects the error codes it reports and the rules
/// that differ between the two (obs-fold, framing).
enum class MessageRole : std::uint8_t
{
    Request,  ///< A request, as a server reads it.
    Response, ///< A response, as a client reads it.
};

/// How a message body is delimited.
enum class BodyFraming : std::uint8_t
{
    None,       ///< There is no body.
    Length,     ///< @c Framing::length bytes follow the head.
    Chunked,    ///< The chunked coding (@c ChunkedDecoder).
    UntilClose, ///< The body ends when the connection does; a response only.
};

/// Whether the connection carries another message after this one.
enum class Persistence : std::uint8_t
{
    KeepAlive, ///< Another message may follow.
    Close,     ///< The connection closes after this message.
};

/// A message's body framing and whether its connection persists.
struct Framing
{
    BodyFraming kind = BodyFraming::None;             ///< How the body is delimited.
    std::uint64_t length = 0;                         ///< The body's length, for @c BodyFraming::Length.
    Persistence persistence = Persistence::KeepAlive; ///< Whether the connection persists.
};

/// The framing of a request with @p fields, by RFC 9112 section 6.3.
///
/// Refuses (@c HttpErrorCode::ProtocolError) what that section and section 6.1 call faulty, and what
/// a smuggler sends: Transfer-Encoding together with Content-Length, Transfer-Encoding in HTTP/1.0,
/// chunked twice or not the final coding, a malformed coding list (an unterminated quote included),
/// and a Content-Length that is not one decimal value (a list of identical values is that value).
/// A request's Transfer-Encoding must be exactly `chunked`: any other coding or a parameter is
/// refused too, as a coding this layer cannot decode (a server answers 501, section 6.1), so no
/// coded body reaches a caller that believes it plain. Every refusal is fatal to the connection: the
/// caller answers it if it can, and closes.
/// @param version The request's version.
/// @param fields The request's fields.
/// @return The framing, or the refusal.
[[nodiscard]] CORE_HTTP_API HttpResult<Framing> requestFraming(HttpVersion version, Fields const& fields);

/// The framing of a response to a @p requestMethod request, by RFC 9112 section 6.3.
///
/// A response to HEAD, a 1xx, 204 or 304 has no body whatever its fields say; a 2xx to CONNECT
/// has none either and closes, since the connection is a tunnel from then on. Transfer-Encoding
/// whose final coding is chunked frames as chunked, and closes if Content-Length came with it; one
/// whose final coding is not chunked reads until close; without either a response reads until
/// close. A transfer coding before the final chunked is not decoded; it stays in the body. The
/// refusals are @c requestFraming's but the two that hold a request to plain chunked and the one
/// for Transfer-Encoding with Content-Length, which a response is framed through.
/// @param requestMethod The method of the request this answers.
/// @param version The response's version.
/// @param status The response's status code.
/// @param fields The response's fields.
/// @return The framing, or the refusal.
[[nodiscard]] CORE_HTTP_API HttpResult<Framing> responseFraming(std::string_view requestMethod,
                                                                HttpVersion version,
                                                                std::uint16_t status,
                                                                Fields const& fields);

/// Whether @p name, compared without regard to case, is a field the connection writes itself:
/// Transfer-Encoding, Content-Length, Connection, Keep-Alive, TE or Upgrade (D7). A @c bool because
/// the name asks the question.
/// @param name A field name.
/// @return True for a framing field.
[[nodiscard]] CORE_HTTP_API bool isFramingField(std::string_view name) noexcept;

/// Whether a parser has reached the end of what it parses.
enum class Progress : std::uint8_t
{
    NeedMore, ///< More bytes are needed.
    Complete, ///< The head, or the body with its trailers, is complete.
};

/// What one @c HeadParser::feed did.
struct FeedResult
{
    Progress progress;    ///< Whether the head is complete.
    std::size_t consumed; ///< How many of the bytes fed belong to the head; the rest are the body's.
};

/// Parses one message head, incrementally and within @c HttpLimits.
///
/// Fed the bytes as they arrive, it holds no more than one line of them, refuses the moment a
/// limit is passed, and never recurses. It refuses what RFC 9112 says a recipient must not accept:
/// a bare LF or a bare CR as a line ending, whitespace before a field's colon or before the first
/// field, an invalid field value, and obs-fold in a request (a response's obs-fold is replaced by
/// SP, as section 5.2 asks of a user agent). A request is also refused for two Host fields, an
/// invalid Host, or none in HTTP/1.1 (section 3.2). Empty lines before a request line are skipped.
/// Once it has refused, it refuses every later feed the same way.
class CORE_HTTP_API HeadParser
{
  public:
    /// A parser for one head of @p role.
    /// @param role What is parsed: a request or a response.
    /// @param limits The bounds the head is held to.
    HeadParser(MessageRole role, HttpLimits limits);

    /// Parses as much of @p bytes as belongs to the head.
    /// @param bytes The next bytes of the message.
    /// @return How many bytes were consumed and whether the head is complete, or the refusal. A
    ///         complete parser consumes nothing more.
    [[nodiscard]] HttpResult<FeedResult> feed(std::string_view bytes);

    /// The parsed request head. Requires a complete request parser; moves the head out.
    [[nodiscard]] RequestHead takeRequest();

    /// The parsed response head. Requires a complete response parser; moves the head out.
    [[nodiscard]] ResponseHead takeResponse();

  private:
    /// Where the parser is.
    enum class State : std::uint8_t
    {
        StartLine, ///< Before the start line ends.
        Fields,    ///< In the fields.
        Complete,  ///< After the empty line.
        Failed,    ///< After a refusal.
    };

    /// Where the parser is within a line.
    enum class LineState : std::uint8_t
    {
        Text, ///< In the line's text.
        Cr,   ///< After its CR, waiting for the LF.
    };

    [[nodiscard]] HttpResult<void> endLine();
    [[nodiscard]] HttpResult<void> startLine(std::string_view line);
    [[nodiscard]] HttpResult<void> fieldLine(std::string_view line);
    [[nodiscard]] HttpResult<void> finish();
    [[nodiscard]] HttpError fail(HttpError error);

    MessageRole _role;
    HttpLimits _limits;
    State _state = State::StartLine;
    LineState _lineState = LineState::Text;
    std::size_t _headBytes = 0;
    std::string _line;
    std::optional<Field> _pending;
    std::optional<HttpError> _error;
    RequestHead _request;
    ResponseHead _response;
};

/// What one @c ChunkedDecoder::feed did.
struct ChunkStep
{
    std::size_t consumed;  ///< How many of the bytes fed were used.
    std::string_view data; ///< Body bytes among them, a view of the bytes fed; may be empty.
    Progress progress;     ///< @c Progress::Complete once the trailers have ended.
};

/// Decodes a chunked body (RFC 9112 section 7.1), incrementally and within @c HttpLimits.
///
/// A table of transitions, indexed by state and character class, drives every byte outside chunk
/// data; data is handed out as views of the bytes fed, at most @c HttpLimits::maxReadChunk at a
/// time. A chunk size has at most 16 hex digits, a chunk line at most @c maxChunkLineBytes, every
/// line ends in CRLF, and the trailers are field lines held to @c maxFieldBytes each and
/// @c maxTrailerBytes together (codes by role). Bytes after the final CRLF are not consumed.
class CORE_HTTP_API ChunkedDecoder
{
  public:
    /// A decoder for one body of @p role.
    /// @param role Whose body is decoded: a request's or a response's.
    /// @param limits The bounds the body is held to.
    ChunkedDecoder(MessageRole role, HttpLimits limits);

    /// Decodes from @p bytes until it has body bytes to hand out, needs more, or is complete.
    /// @param bytes The next bytes of the body.
    /// @return What was consumed and decoded, or the refusal, which every later feed repeats.
    [[nodiscard]] HttpResult<ChunkStep> feed(std::string_view bytes);

    /// The trailer fields; complete once @c feed has reported @c Progress::Complete. Moves them out.
    [[nodiscard]] Fields takeTrailers();

    /// Where the decoder is; the rows of its transition table.
    enum class State : std::uint8_t
    {
        Size,        ///< In a chunk size.
        SizeSpace,   ///< In whitespace after the size, which only a ';' may follow.
        Extension,   ///< In a chunk's extensions.
        SizeLf,      ///< After a chunk line's CR.
        Data,        ///< In chunk data.
        DataCr,      ///< After chunk data, before its CR.
        DataLf,      ///< After chunk data's CR.
        TrailerLine, ///< In a trailer line, or at the empty line that ends the body.
        TrailerLf,   ///< After a trailer line's CR.
        Done,        ///< After the final CRLF.
        Failed,      ///< After a refusal.
    };

  private:
    [[nodiscard]] HttpResult<void> step(char octet);
    [[nodiscard]] HttpResult<void> endTrailerLine();
    [[nodiscard]] HttpError fail(HttpError error);

    MessageRole _role;
    HttpLimits _limits;
    State _state = State::Size;
    std::uint64_t _size = 0;
    std::size_t _sizeDigits = 0;
    std::size_t _lineBytes = 0;
    std::uint64_t _remaining = 0;
    std::uint64_t _bodyBytes = 0;
    std::size_t _trailerBytes = 0;
    std::string _line;
    std::optional<Field> _pending;
    std::optional<HttpError> _error;
    Fields _trailers;
};

/// The line that opens a chunk of @p size bytes: lower-case hex and CRLF, `1a\r\n` for 26.
/// @param size The chunk's size; nonzero, since a zero-size chunk is @c lastChunk.
/// @return The chunk line.
[[nodiscard]] CORE_HTTP_API std::string chunkHeader(std::size_t size);

/// The last chunk and the trailer section that end a chunked body: `0\r\n`, each trailer, `\r\n`.
/// @param trailers The trailer fields; validated already, as every @c Fields is.
/// @return The bytes that end the body.
[[nodiscard]] CORE_HTTP_API std::string lastChunk(Fields const& trailers);

} // namespace core::http
