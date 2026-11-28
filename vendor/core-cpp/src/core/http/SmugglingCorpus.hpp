// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The request-smuggling corpus: requests whose framing RFC 9112 says to refuse, beside the ones it
/// says to accept, each with the error and the status a server answers. Run here through
/// @c HeadParser, @c requestFraming and @c ChunkedDecoder (Framing_test.cpp), and by the server's
/// tests (Task 18) over a connection, both under @c smugglingLimits(). Test-only: in no
/// `FILE_SET`, so never installed.

#include <core/http/Framing.hpp>
#include <core/http/Message.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace core::http::test
{

/// Whether a corpus message is to be refused or read.
enum class SmugglingVerdict : std::uint8_t
{
    Refused,  ///< The message is refused with @c SmugglingCase::code and @c SmugglingCase::status.
    Accepted, ///< The message is read; its body is @c SmugglingCase::body.
};

/// One message of the corpus.
struct SmugglingCase
{
    std::string_view name;    ///< What the row tests, unique, usable as a section name.
    MessageRole role;         ///< A request (a server reads it) or a response (a client does).
    std::string_view wire;    ///< The message's bytes, as its sender sends them.
    SmugglingVerdict verdict; ///< Refused or accepted.
    HttpErrorCode code;       ///< The error a refused message is refused with; unused when accepted.
    /// What a server answers a request row with (200 when accepted); for a response row, what a
    /// gateway that received it answers its own client with (502 when refused).
    std::uint16_t status;
    std::string_view body; ///< The decoded body of an accepted message.
};

/// The limits the corpus is written against: small enough that a size row is a short literal.
[[nodiscard]] inline HttpLimits smugglingLimits()
{
    return HttpLimits { .maxHeadBytes = 1024,
                        .maxFieldBytes = 256,
                        .maxFields = 100,
                        .maxTrailerBytes = 256,
                        .maxChunkLineBytes = 64,
                        .maxReadChunk = std::size_t { 64 } * 1024,
                        .maxBodyBytes = std::nullopt };
}

namespace detail
{
    using namespace std::string_view_literals;

    inline constexpr auto SmugglingRows = std::array {
        // RFC 9112 6.1: Transfer-Encoding and Content-Length together; the server may reject, and does
        SmugglingCase { .name = "te-with-cl",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 3\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 6.3 item 5: an invalid Content-Length
        SmugglingCase { .name = "cl-two-different",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 3\r\n"
                                "Content-Length: 4\r\n"
                                "\r\n"
                                "abcd"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: a list of differing values
        SmugglingCase { .name = "cl-list-differs",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 3, 4\r\n"
                                "\r\n"
                                "abcd"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: Content-Length = 1*DIGIT
        SmugglingCase { .name = "cl-plus-sign",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: +3\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: Content-Length = 1*DIGIT
        SmugglingCase { .name = "cl-minus-sign",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: -3\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: whitespace inside the value
        SmugglingCase { .name = "cl-inner-space",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 1 2\r\n"
                                "\r\n"
                                "abcdefghijkl"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: Content-Length = 1*DIGIT
        SmugglingCase { .name = "cl-hex",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 0x3\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: Content-Length = 1*DIGIT
        SmugglingCase { .name = "cl-empty",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: \r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: a length past 2^64 - 1 cannot be represented
        SmugglingCase { .name = "cl-overflow",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 18446744073709551616\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 8.6: identical values are one value
        SmugglingCase { .name = "cl-identical-duplicate",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 3\r\n"
                                "Content-Length: 3\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "abc" },
        // RFC 9110 8.6: a list of identical decimal values is one value
        SmugglingCase { .name = "cl-identical-list",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 3, 003\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "abc" },
        // RFC 9112 7: chunked is applied at most once
        SmugglingCase { .name = "te-chunked-twice",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked, chunked\r\n"
                                "\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7: chunked is applied at most once, across field lines too
        SmugglingCase { .name = "te-chunked-twice-lines",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 6.3 item 4: chunked is not the final request coding
        SmugglingCase { .name = "te-chunked-then-gzip",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked, gzip\r\n"
                                "\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 6.3 item 4: chunked is not the final request coding
        SmugglingCase { .name = "te-not-chunked",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: gzip\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 6.1: a Transfer-Encoding with no coding
        SmugglingCase { .name = "te-empty",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: \r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7: chunked takes no parameter
        SmugglingCase { .name = "te-chunked-with-parameter",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked;q=1\r\n"
                                "\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 6.1: Transfer-Encoding in HTTP/1.0 is faulty framing; the connection closes
        SmugglingCase { .name = "te-on-http10",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.0\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 5.1: whitespace between the field name and the colon
        SmugglingCase { .name = "te-space-before-colon",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding : chunked\r\n"
                                "Content-Length: 3\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 5.1: whitespace between the field name and the colon
        SmugglingCase { .name = "cl-space-before-colon",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length : 3\r\n"
                                "\r\n"
                                "abc"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 3.2: more than one Host field line
        SmugglingCase { .name = "host-twice",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Host: b\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 3.2: an HTTP/1.1 request without Host
        SmugglingCase { .name = "host-missing-http11",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 3.2: only HTTP/1.1 requires Host
        SmugglingCase { .name = "host-missing-http10",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "" },
        // RFC 9112 3.2: an invalid Host value
        SmugglingCase { .name = "host-invalid",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a/b\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 5.2: a server rejects obs-fold
        SmugglingCase { .name = "obs-fold",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "X-A: 1\r\n"
                                " 2\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 2.2: whitespace between the start line and the first field
        SmugglingCase { .name = "whitespace-before-first-field",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                " Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 2.2: a bare LF line ending is refused, not recognized
        SmugglingCase { .name = "bare-lf",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\n"
                                "Host: a\n"
                                "\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 2.2: a bare LF line ending is refused, not recognized
        SmugglingCase { .name = "bare-lf-in-fields",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a\n"
                                "X-A: b\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 2.2: a bare CR is invalid
        SmugglingCase { .name = "bare-cr",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a\rX-A: b\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 5.5: a NUL in a field value
        SmugglingCase { .name = "nul-in-value",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "X-A: a\0b\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 2.3: only major version 1 is spoken
        SmugglingCase { .name = "http-version-2",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/2.0\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 505,
                        .body = "" },
        // a chunk size past 16 hex digits could overflow 64 bits
        SmugglingCase { .name = "chunk-size-17-digits",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "00000000000000001\r\n"
                                "a\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // 16 hex digits fit; leading zeros are digits
        SmugglingCase { .name = "chunk-size-16-digits",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "0000000000000001\r\n"
                                "a\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "a" },
        // RFC 9112 7.1: a chunk line ends in CRLF
        SmugglingCase { .name = "chunk-size-bare-lf",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7.1: chunk data is followed by CRLF
        SmugglingCase { .name = "chunk-data-overrun",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abcd\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7.1: whitespace after the size is BWS before a ";" only
        SmugglingCase { .name = "chunk-size-trailing-space",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3 \r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7.1: chunk-size = 1*HEXDIG, so an empty chunk line is no size of 0
        SmugglingCase { .name = "chunk-size-empty",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7.1: chunk-size = 1*HEXDIG
        SmugglingCase { .name = "chunk-size-not-hex",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3g\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7.1.1: extensions and trailers are accepted
        SmugglingCase { .name = "chunk-extension",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3 ; name=\"v a\"\r\n"
                                "abc\r\n"
                                "0;x\r\n"
                                "X-T: 1\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "abc" },
        // RFC 9112 6.1: a request coding this layer cannot decode; a server answers 501
        SmugglingCase { .name = "te-gzip-chunked",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: gzip, chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 501,
                        .body = "" },
        // a quoted comma does not split a coding list; the unterminated quote is malformed
        SmugglingCase { .name = "te-unterminated-quote",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: gzip;a=\"x, chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 6.1: a request transfer coding with a parameter; a server answers 501
        SmugglingCase { .name = "te-parameter",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: gzip;q=1, chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 501,
                        .body = "" },
        // RFC 9112 6.1: a quoted parameter that hides a comma; a server answers 501
        SmugglingCase { .name = "te-quoted-parameter",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: gzip;a=\"x, y\", chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 501,
                        .body = "" },
        // RFC 9112 7.1: a bare LF ends a chunk extension
        SmugglingCase { .name = "chunk-extension-bare-lf",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3;a\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7.1: a bare CR inside a chunk extension
        SmugglingCase { .name = "chunk-extension-bare-cr",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3;a\rz\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9110 5.5: a NUL inside a chunk extension
        SmugglingCase { .name = "chunk-extension-nul",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3;a\0b\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 7.1: chunk data ends in CRLF, not a bare LF
        SmugglingCase { .name = "chunk-data-bare-lf",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 2.2: a trailer line ending in a bare LF
        SmugglingCase { .name = "trailer-bare-lf",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "0\r\n"
                                "X-T: 1\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // RFC 9112 2.2: a bare LF line ending in a response head
        SmugglingCase { .name = "response-bare-lf",
                        .role = MessageRole::Response,
                        .wire = "HTTP/1.1 200 OK\n"
                                "Content-Length: 0\n"
                                "\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 502,
                        .body = "" },
        // RFC 9110 5.5: a NUL in a response field value
        SmugglingCase { .name = "response-nul-in-value",
                        .role = MessageRole::Response,
                        .wire = "HTTP/1.1 200 OK\r\n"
                                "X-A: a\0b\r\n"
                                "Content-Length: 0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 502,
                        .body = "" },
        // RFC 9112 6.3 item 3: a response with TE and CL frames as chunked (and closes)
        SmugglingCase { .name = "response-te-with-cl",
                        .role = MessageRole::Response,
                        .wire = "HTTP/1.1 200 OK\r\n"
                                "Content-Length: 99\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "abc" },
        // RFC 9112 6.3 item 5: a response with two different lengths
        SmugglingCase { .name = "response-cl-two-different",
                        .role = MessageRole::Response,
                        .wire = "HTTP/1.1 200 OK\r\n"
                                "Content-Length: 3\r\n"
                                "Content-Length: 4\r\n"
                                "\r\n"
                                "abcd"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 502,
                        .body = "" },
        // RFC 9110 5.6.4: an escaped quote does not end a quoted-string, so the comma after it is
        // inside the string and the list ends unterminated
        SmugglingCase { .name = "response-te-escaped-quote",
                        .role = MessageRole::Response,
                        .wire = "HTTP/1.1 200 OK\r\n"
                                "Transfer-Encoding: gzip;a=\"x\\\", chunked\r\n"
                                "\r\n"
                                "3\r\n"
                                "abc\r\n"
                                "0\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 502,
                        .body = "" },
        // the head is past HttpLimits::maxHeadBytes
        SmugglingCase { .name = "head-too-large",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "X-Pad-0: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                                "X-Pad-1: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                                "X-Pad-2: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                                "X-Pad-3: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                                "X-Pad-4: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestHeaderSectionSize,
                        .status = 431,
                        .body = "" },
        // RFC 9112 3.2.3: the authority-form is only used for CONNECT
        SmugglingCase { .name = "target-authority-with-get",
                        .role = MessageRole::Request,
                        .wire = "GET a:80 HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestUriInvalid,
                        .status = 400,
                        .body = "" },
        // RFC 9112 3.2.3: not even with OPTIONS
        SmugglingCase { .name = "target-authority-with-options",
                        .role = MessageRole::Request,
                        .wire = "OPTIONS a:80 HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestUriInvalid,
                        .status = 400,
                        .body = "" },
        // RFC 9112 3.2.3: CONNECT names an authority and nothing else
        SmugglingCase { .name = "target-connect-origin-form",
                        .role = MessageRole::Request,
                        .wire = "CONNECT / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestUriInvalid,
                        .status = 400,
                        .body = "" },
        // RFC 9110 9.3.6: CONNECT's target is an authority
        SmugglingCase { .name = "target-connect-absolute-form",
                        .role = MessageRole::Request,
                        .wire = "CONNECT http://a/ HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestUriInvalid,
                        .status = 400,
                        .body = "" },
        // RFC 9112 3.2.4: the asterisk-form is only used for a server-wide OPTIONS
        SmugglingCase { .name = "target-asterisk-with-get",
                        .role = MessageRole::Request,
                        .wire = "GET * HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestUriInvalid,
                        .status = 400,
                        .body = "" },
        // RFC 9112 3.2.3: CONNECT with the authority-form
        SmugglingCase { .name = "target-connect-authority",
                        .role = MessageRole::Request,
                        .wire = "CONNECT a:443 HTTP/1.1\r\n"
                                "Host: a:443\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "" },
        // RFC 9112 3.2.4: OPTIONS with the asterisk-form
        SmugglingCase { .name = "target-options-asterisk",
                        .role = MessageRole::Request,
                        .wire = "OPTIONS * HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "" },
        // RFC 9110 9.3.7: OPTIONS may name a resource too
        SmugglingCase { .name = "target-options-origin-form",
                        .role = MessageRole::Request,
                        .wire = "OPTIONS /x HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "" },
        // RFC 9112 3.2.2: a server accepts the absolute-form
        SmugglingCase { .name = "target-absolute-form",
                        .role = MessageRole::Request,
                        .wire = "GET http://a/x HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Accepted,
                        .code = HttpErrorCode::InternalError,
                        .status = 200,
                        .body = "" },
        // CL.TE: a front end framing by Content-Length would pass the chunked body's end and the request
        // behind it as one body; refused at the head, so nothing after it is ever read as a request
        SmugglingCase { .name = "smuggled-get-behind-te-with-cl",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 44\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "\r\n"
                                "0\r\n"
                                "\r\n"
                                "GET /smuggled HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // two Content-Length values: one reader takes the shorter, and the rest is a request
        SmugglingCase { .name = "smuggled-get-behind-two-lengths",
                        .role = MessageRole::Request,
                        .wire = "POST / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "Content-Length: 0\r\n"
                                "Content-Length: 37\r\n"
                                "\r\n"
                                "GET /smuggled HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::ProtocolError,
                        .status = 400,
                        .body = "" },
        // a field line is past HttpLimits::maxFieldBytes
        SmugglingCase { .name = "field-too-large",
                        .role = MessageRole::Request,
                        .wire = "GET / HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "X-Big: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestHeaderSize,
                        .status = 431,
                        .body = "" },
        // the request line is past HttpLimits::maxFieldBytes
        SmugglingCase { .name = "request-line-too-long",
                        .role = MessageRole::Request,
                        .wire = "GET /ccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
                                "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
                                "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
                                "ccccccccccccccccccccccccccccccccccccccccccccccccc HTTP/1.1\r\n"
                                "Host: a\r\n"
                                "\r\n"sv,
                        .verdict = SmugglingVerdict::Refused,
                        .code = HttpErrorCode::RequestUriTooLong,
                        .status = 414,
                        .body = "" },
    };
} // namespace detail

/// Every row of the corpus.
[[nodiscard]] constexpr std::span<SmugglingCase const> smugglingCorpus() noexcept
{
    return detail::SmugglingRows;
}

} // namespace core::http::test
