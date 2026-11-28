// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The HTTP/1.1 message vocabulary (RFC 9110, RFC 9112): the error codes with their payload, the
/// field list, and the validating factories every method, request target and reason phrase is built
/// through, so no value that could split or smuggle a message reaches the wire.

#include <core/http/HttpExport.hpp>
#include <core/net/ITlsSession.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::http
{

/// The protocol version a message is framed by.
enum class HttpVersion : std::uint8_t
{
    Http10, ///< HTTP/1.0: no chunked coding, closes unless asked to keep alive.
    Http11, ///< HTTP/1.1, and every later 1.x minor version a peer sends (RFC 9112 section 2.3).
};

/// Classifies an HTTP failure.
///
/// One code for each case of the WASI 0.3 `wasi:http` `error-code`, in its order, so a consumer
/// maps them one to one; the payload a case carries is in @c HttpError. @c Last is a sentinel, the
/// number of codes, and never an error.
enum class HttpErrorCode : std::uint8_t
{
    DnsTimeout,                 ///< The name lookup timed out.
    DnsError,                   ///< The name lookup failed (@c HttpError::dnsRcode, @c dnsInfoCode).
    DestinationNotFound,        ///< The name does not exist.
    DestinationUnavailable,     ///< The destination exists but cannot be reached now.
    DestinationIpProhibited,    ///< The destination address is not allowed.
    DestinationIpUnroutable,    ///< There is no route to the destination address.
    ConnectionRefused,          ///< The destination refused the connection.
    ConnectionTerminated,       ///< The connection ended before the exchange did.
    ConnectionTimeout,          ///< Connecting timed out.
    ConnectionReadTimeout,      ///< A read timed out between bytes.
    ConnectionWriteTimeout,     ///< A write timed out.
    ConnectionLimitReached,     ///< No connection could be had within the configured cap.
    TlsProtocolError,           ///< The TLS handshake or record layer failed.
    TlsCertificateError,        ///< The peer's certificate was refused.
    TlsAlertReceived,           ///< The peer sent a TLS alert (@c HttpError::tls).
    RequestDenied,              ///< The request was refused by policy.
    RequestLengthRequired,      ///< The request needs a length it does not carry.
    RequestBodySize,            ///< The request body is too large, or not its declared size (@c size).
    RequestMethodInvalid,       ///< The method is not a token.
    RequestUriInvalid,          ///< The request target is not one of the four forms.
    RequestUriTooLong,          ///< The request line is too long.
    RequestHeaderSectionSize,   ///< The request head is too large (@c size).
    RequestHeaderSize,          ///< A request field line is too large (@c fieldName, @c size).
    RequestTrailerSectionSize,  ///< The request trailer section is too large (@c size).
    RequestTrailerSize,         ///< A request trailer line is too large (@c fieldName, @c size).
    ResponseIncomplete,         ///< The response ended before its framing did.
    ResponseHeaderSectionSize,  ///< The response head is too large (@c size).
    ResponseHeaderSize,         ///< A response field line is too large (@c fieldName, @c size).
    ResponseBodySize,           ///< The response body is too large, or not its declared size (@c size).
    ResponseTrailerSectionSize, ///< The response trailer section is too large (@c size).
    ResponseTrailerSize,        ///< A response trailer line is too large (@c fieldName, @c size).
    ResponseTransferCoding,     ///< A transfer coding cannot be decoded (@c coding).
    ResponseContentCoding,      ///< A content coding cannot be decoded (@c coding).
    ResponseTimeout,            ///< No response head arrived in time.
    UpgradeFailed,              ///< A protocol upgrade failed.
    ProtocolError,              ///< The peer, or the caller, broke the protocol's syntax or framing.
    LoopDetected,               ///< A redirect or proxy loop.
    ConfigurationError,         ///< The client or server is configured so it cannot do this.
    InternalError,              ///< Anything else (@c detail).
    Last,                       ///< The number of codes; never an error.
};

namespace detail
{
    /// The name of each code, indexed by its value.
    inline constexpr auto HttpErrorCodeNames =
        std::array<std::string_view, static_cast<std::size_t>(HttpErrorCode::Last)> {
            "DNS timeout",
            "DNS error",
            "destination not found",
            "destination unavailable",
            "destination IP prohibited",
            "destination IP unroutable",
            "connection refused",
            "connection terminated",
            "connection timeout",
            "connection read timeout",
            "connection write timeout",
            "connection limit reached",
            "TLS protocol error",
            "TLS certificate error",
            "TLS alert received",
            "request denied",
            "request length required",
            "request body size",
            "request method invalid",
            "request URI invalid",
            "request URI too long",
            "request header section size",
            "request header size",
            "request trailer section size",
            "request trailer size",
            "response incomplete",
            "response header section size",
            "response header size",
            "response body size",
            "response trailer section size",
            "response trailer size",
            "response transfer coding",
            "response content coding",
            "response timeout",
            "upgrade failed",
            "protocol error",
            "loop detected",
            "configuration error",
            "internal error",
        };
} // namespace detail

/// A short, human-readable name for @p code; "unknown" for @c HttpErrorCode::Last.
[[nodiscard]] constexpr std::string_view toString(HttpErrorCode code) noexcept
{
    auto const index = static_cast<std::size_t>(code);
    return index < detail::HttpErrorCodeNames.size() ? detail::HttpErrorCodeNames.at(index) : "unknown";
}

/// What a refusal of a peer's message was for, where its code alone does not say which status a
/// server answers it with.
///
/// Not part of the WIT's `error-code`, which has no case for any of these: the first two are a
/// @c HttpErrorCode::ProtocolError there, and the third a read timeout. RFC 9110 gives the first two a
/// status of their own, and a server closes an idle connection without one (RFC 9112 section 9.5), so
/// the parsers and the connection say which it was and a server's @c errorStatusTable maps the pair.
enum class RefusalReason : std::uint8_t
{
    Unclassified,        ///< Nothing beyond the code; the default.
    UnsupportedVersion,  ///< The message's version is not HTTP/1.x (RFC 9110 section 15.6.6: 505).
    UnimplementedCoding, ///< A request transfer coding this layer cannot decode (RFC 9112 6.1: 501).
    NothingArrived,      ///< A read deadline passed before a message's first byte: an idle connection.
};

/// An HTTP failure: its code and the payload the code carries.
///
/// The payload fields follow the pinned WIT's payload types; each is set only for the codes that
/// name it in @c HttpErrorCode, and empty otherwise. @c reason refines a refusal beyond the WIT.
struct HttpError
{
    HttpErrorCode code = HttpErrorCode::InternalError;  ///< What failed.
    std::optional<std::uint64_t> size;                  ///< The size past a limit, or the actual body size.
    std::optional<std::string> fieldName;               ///< The field a field-size code is about.
    std::optional<std::string> dnsRcode;                ///< The DNS response code's name (@c DnsError).
    std::optional<std::uint16_t> dnsInfoCode;           ///< The extended DNS error code (@c DnsError).
    std::optional<std::string> coding;                  ///< The coding a coding code is about.
    std::optional<net::TlsFailure> tls;                 ///< The TLS failure (@c TlsAlertReceived and others).
    std::string detail;                                 ///< A human-readable reason; may be empty.
    RefusalReason reason = RefusalReason::Unclassified; ///< What a refusal was for, beyond the code.

    /// The code's name, then whatever of the payload is set.
    [[nodiscard]] CORE_HTTP_API std::string toString() const;
};

/// The result of an HTTP operation.
template <typename T>
using HttpResult = std::expected<T, HttpError>;

/// One field line: a name and its value, as given (names keep their case).
struct Field
{
    std::string name;  ///< The field name, a token.
    std::string value; ///< The field value, without leading or trailing whitespace.
};

/// An ordered list of field lines: a head's fields or a body's trailers.
///
/// Every entry was validated on the way in, so a @c Fields is safe to write as it is. It refuses no
/// name for what the name means: framing fields such as Content-Length are legal entries, and
/// keeping a caller's out of a message is the writer's rule (@c isFramingField), not this type's.
class CORE_HTTP_API Fields
{
  public:
    /// Appends one field line.
    ///
    /// Refuses a name that is not a token (RFC 9110 section 5.1), and a value with CR, LF, NUL, any
    /// other control character but HTAB, or leading or trailing whitespace (section 5.5).
    /// @param name The field name.
    /// @param value The field value.
    /// @return Nothing, or @c HttpErrorCode::ProtocolError naming the field; nothing is appended then.
    [[nodiscard]] HttpResult<void> append(std::string name, std::string value);

    /// Every value of the field named @p name, compared without regard to case, in order.
    /// @param name The field name.
    /// @return Views of the values, valid until this list changes.
    [[nodiscard]] std::vector<std::string_view> get(std::string_view name) const;

    /// Removes every field line named @p name, compared without regard to case.
    /// @param name The field name.
    /// @return How many lines were removed.
    std::size_t erase(std::string_view name);

    /// Every field line, in order.
    [[nodiscard]] std::span<Field const> entries() const noexcept { return _entries; }

  private:
    std::vector<Field> _entries;
};

/// A request method: a token (RFC 9110 section 9.1), case-sensitive.
class CORE_HTTP_API Method
{
  public:
    /// GET.
    Method();

    /// Validates @p token as a method.
    /// @param token The method, as it appears on the request line.
    /// @return The method, or @c HttpErrorCode::RequestMethodInvalid.
    [[nodiscard]] static HttpResult<Method> parse(std::string_view token);

    /// The method's text.
    [[nodiscard]] std::string_view text() const noexcept { return _text; }

  private:
    explicit Method(std::string text);

    std::string _text;
};

/// Which of the four forms of RFC 9112 section 3.2 a request target has.
enum class RequestTargetForm : std::uint8_t
{
    Origin,    ///< An absolute path and an optional query: `/where?q`.
    Absolute,  ///< An absolute URI: `http://host/where`, as sent to a proxy.
    Authority, ///< A host and a port: `host:443`, for CONNECT.
    Asterisk,  ///< `*`, for a server-wide OPTIONS.
};

/// A request target, validated to be one of the four forms and to hold only URI characters.
class CORE_HTTP_API RequestTarget
{
  public:
    /// `/`.
    RequestTarget();

    /// Validates @p target.
    ///
    /// Refuses whitespace, control characters, octets outside ASCII, a fragment, a malformed
    /// percent-encoding, and anything that is not one of the four forms.
    /// @param target The target, as it appears on the request line.
    /// @return The target, or @c HttpErrorCode::RequestUriInvalid.
    [[nodiscard]] static HttpResult<RequestTarget> parse(std::string_view target);

    /// The target's text.
    [[nodiscard]] std::string_view text() const noexcept { return _text; }

    /// The target's form.
    [[nodiscard]] RequestTargetForm form() const noexcept { return _form; }

  private:
    RequestTarget(std::string text, RequestTargetForm form);

    std::string _text;
    RequestTargetForm _form;
};

/// A status line's reason phrase: HTAB, SP, visible ASCII and obs-text (RFC 9112 section 4).
class CORE_HTTP_API ReasonPhrase
{
  public:
    /// The empty phrase.
    ReasonPhrase() = default;

    /// Validates @p text.
    /// @param text The phrase; may be empty.
    /// @return The phrase, or @c HttpErrorCode::ProtocolError for a control character but HTAB.
    [[nodiscard]] static HttpResult<ReasonPhrase> parse(std::string_view text);

    /// The phrase's text.
    [[nodiscard]] std::string_view text() const noexcept { return _text; }

  private:
    explicit ReasonPhrase(std::string text);

    std::string _text;
};

/// A request's start line and its fields.
struct RequestHead
{
    Method method;                             ///< The method.
    RequestTarget target;                      ///< The request target.
    HttpVersion version = HttpVersion::Http11; ///< The version it is framed by.
    Fields fields;                             ///< The header fields.
};

/// A response's status line and its fields.
struct ResponseHead
{
    std::uint16_t status = 200;                ///< The status code; one outside 100 to 999 is never written.
    ReasonPhrase reason;                       ///< The reason phrase.
    HttpVersion version = HttpVersion::Http11; ///< The version it is framed by.
    Fields fields;                             ///< The header fields.
};

} // namespace core::http
