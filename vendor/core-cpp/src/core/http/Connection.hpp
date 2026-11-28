// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One HTTP/1.1 connection over an @c net::ISocket: heads and bodies read through the pure layer
/// (@c HeadParser, @c ChunkedDecoder) out of one read buffer of @c HttpLimits::maxReadChunk bytes,
/// heads and bodies written with the framing fields the connection adds itself, and whether the
/// connection may carry another message. The same type serves both ends; its @c MessageRole is what
/// it reads, which selects the error codes.

#include <core/async/Task.hpp>
#include <core/http/Body.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpExport.hpp>
#include <core/http/Message.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>

#include <chrono>
#include <cstdint>
#include <memory>

namespace core::http
{

namespace detail
{
    struct ConnectionState;
} // namespace detail

/// Whether a connection may carry another message.
enum class Reuse : std::uint8_t
{
    Yes, ///< The last exchange ended cleanly and both ends keep the connection.
    No,  ///< It must be closed.
};

/// The deadlines of a connection, on its loop's clock.
///
/// **A zero duration means no deadline** (R31): that wait is not bounded by the connection at all,
/// rather than bounded by a deadline that fires at once.
///
/// A read deadline retires the parked read through @c net::ISocket::cancelRead and leaves the
/// socket open, so a server can still answer 408; **a socket decorator that does not forward
/// `cancelRead` therefore disables the read deadlines.** A write deadline closes the socket, since a
/// write cut off mid-message leaves nothing worth keeping it open for.
struct WireTimeouts
{
    /// From the start of a head read to its first byte. In the response role this is the response
    /// timeout (@c HttpErrorCode::ResponseTimeout); in the request role it is the idle time a client
    /// may take before its next request (@c HttpErrorCode::ConnectionReadTimeout).
    ///
    /// **A request still being written counts toward it**, as `wasi:http`'s first-byte timeout does
    /// in wasmtime: a head read alongside an upload times out after this long whether or not the
    /// upload is done, which also bounds a server that never reads the body.
    std::chrono::milliseconds firstByte { 0 };
    /// Between any two reads after that, the body's included (@c HttpErrorCode::ConnectionReadTimeout).
    std::chrono::milliseconds betweenBytes { 0 };
    /// Each write, a head's or a body chunk's, to its last byte (@c HttpErrorCode::ConnectionWriteTimeout).
    /// A peer that stops reading stalls a write at most this long.
    std::chrono::milliseconds write { 0 };
    /// A head as a whole, from its first byte to its last (@c HttpErrorCode::ConnectionReadTimeout): the
    /// wait for that first byte is @c firstByte's alone, a separate budget. It is checked before every
    /// read of the head after the first byte and caps each read's own deadline, so neither a trickle
    /// of bytes each inside @c betweenBytes nor a read that beat its cancel by a byte (a completion
    /// transport's, where the data wins) stretches a head past it.
    std::chrono::milliseconds head { 0 };
    /// A body as a whole, from its reader's first adoption after the head to its last byte
    /// (@c HttpErrorCode::ConnectionReadTimeout), checked before every read of it as @c head is. Time
    /// the caller spends between reads counts: a read after the bound is refused at once.
    std::chrono::milliseconds body { 0 };
};

/// One HTTP/1.1 connection, either end.
///
/// It holds one read and one write operation at a time, as the socket does: a head read and a body
/// read share the read side, and a head write and a body write the write side, so one read flow
/// and one write flow may run at once. A body reader shares the connection's state and may outlive
/// the @c Connection. Every failure, the peer's or the socket's, leaves the connection
/// @c Reuse::No, and so does a flow cancelled while it waited on the socket, whose message is cut
/// off at an unknown byte. Loop-affine: every call is made on the loop's thread.
///
/// **The read side runs head, reader, head.** After every head read the caller frames it and asks
/// @c bodyReader for its body, a @c BodyFraming::None one included, before the next head read; only
/// an interim (1xx) response head needs none. Reading a head while the last one's body is unread
/// is a contract violation (`Require`), since the connection cannot know where that body ends.
/// A connection that announced or received @c Persistence::Close still finishes its exchange: a
/// client that sent `Connection: close` reads the response; only the next message is ruled out,
/// and a head read after that exchange's last message answers @c ConnectionTerminated.
///
/// **The request role is serial.** A server reads request N+1's head only once response N is
/// written in full; asked earlier, the head read answers @c ProtocolError, as a value and without
/// consuming a byte, so the caller may answer and ask again. HTTP/1.1 lets a server answer
/// pipelined requests one at a time, and a WASI handler serves one request per connection at a
/// time, so the connection keeps one request's state (whether it was HEAD) rather than a queue.
///
/// **Failures are kept per direction.** A failure on the read side -- a refused head, a read
/// timeout, a cancelled read -- leaves the write side free to answer (400, 408); a failure on the
/// write side stops further writes. **A refusal before the first wire byte leaves the connection as
/// it was; a failure after it breaks that direction:** a head refused for a caller framing field or
/// a framing its status cannot carry may be followed by a corrected head, while every failure of a
/// body write, and of any write once it reached the socket, ends the write side. A flow cancelled while it
/// waited on the socket breaks its side for good: the next operation on that side, a retry included, is
/// refused with
/// @c ConnectionTerminated.
class CORE_HTTP_API Connection
{
  public:
    /// @param loop The loop whose clock the deadlines are measured on (not owned; outlives this and
    ///        every body reader).
    /// @param socket The connected socket, owned from now on.
    /// @param role What this end READS: Response on a client, Request on a server; selects the
    ///        error codes.
    /// @param limits The bounds every message read is held to; its @c maxReadChunk is also the read
    ///        buffer's size, the most the connection ever holds of a peer's bytes.
    /// @param timeouts The read deadlines; zero is none.
    Connection(net::EventLoop& loop,
               std::unique_ptr<net::ISocket> socket,
               MessageRole role,
               HttpLimits limits,
               WireTimeouts timeouts);
    ~Connection();
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    /// Reads the next request head. Requires the request role, and that the previous body has been
    /// read to its end. A HEAD request makes the response written next bodiless whatever its
    /// framing announces.
    /// @return The head, or the failure: the role's limit and syntax codes, @c ProtocolError for a
    ///         close inside the head, @c ConnectionTerminated for one before its first byte, and
    ///         @c ConnectionReadTimeout.
    [[nodiscard]] async::Task<HttpResult<RequestHead>> readRequestHead();

    /// Reads the next response head. Requires the response role, and that the previous body has
    /// been read to its end.
    /// @return The head, or the failure: as @c readRequestHead, with @c ResponseIncomplete for a close
    ///         inside the head and @c ResponseTimeout before its first byte.
    [[nodiscard]] async::Task<HttpResult<ResponseHead>> readResponseHead();

    /// A reader of the body that follows the last head read, framed by @p framing (from
    /// @c requestFraming or @c responseFraming). It shares the connection state; one at a time. A
    /// reader dropped before the end leaves the rest unread, and a later reader with the same
    /// framing continues where it stopped. A Length body past @c HttpLimits::maxBodyBytes fails its
    /// first read with the role's body-size code.
    /// @param framing The body's framing; adopted with the first reader after a head.
    /// @return The reader.
    [[nodiscard]] std::unique_ptr<IBodyReader> bodyReader(Framing framing);

    /// Whether a body reader of this connection is alive. A @c bool because the name asks it.
    [[nodiscard]] bool hasLiveBodyReader() const noexcept;

    /// Writes a request head, one @c writeVectored. Adds the framing fields itself; a
    /// caller-supplied framing field (@c isFramingField) is refused (ProtocolError) before a byte
    /// is written, as are @c BodyFraming::UntilClose and chunked in HTTP/1.0. Requires the response
    /// role, whose end writes requests.
    /// @param head The head.
    /// @param framing The framing @c writeBody will follow.
    /// @return Nothing, or the failure.
    [[nodiscard]] async::Task<HttpResult<void>> writeRequestHead(RequestHead head, Framing framing);

    /// Writes a response head, one @c writeVectored, as @c writeRequestHead does. A 1xx or 204 refuses
    /// a body framing and gets no framing field; a 304 may announce a Length (RFC 9110 section 8.6)
    /// and sends no body; any other status with @c BodyFraming::None is sent `Content-Length: 0`, and
    /// @c BodyFraming::UntilClose is sent `Connection: close`. The response to a HEAD request
    /// announces its framing as given and sends no body (RFC 9110 section 9.3.2), so @c writeBody
    /// after it writes nothing. Requires the request role, and no body still pending from the last
    /// head written.
    /// @param head The head.
    /// @param framing The framing @c writeBody will follow.
    /// @return Nothing, or the failure.
    [[nodiscard]] async::Task<HttpResult<void>> writeResponseHead(ResponseHead head, Framing framing);

    /// Writes a body, pulling it from @p source a chunk at a time, framed by @p framing.
    ///
    /// A body longer or shorter than a Length framing is RequestBodySize/ResponseBodySize (by what
    /// this end writes) and Reuse::No, and no byte past the length is written: a known length that
    /// differs is refused before any body byte, and a streamed one at the chunk that would pass it.
    /// Trailers need @c BodyFraming::Chunked and may hold no framing field (ProtocolError). An
    /// @c BodyFraming::UntilClose body ends when the caller closes. @p framing must be the one the
    /// last head written announced (ProtocolError otherwise); without a head, only none.
    /// @param source The body (not owned; outlives the call), or null for none.
    /// @param framing The framing the head announced.
    /// @return Nothing, or the failure.
    [[nodiscard]] async::Task<HttpResult<void>> writeBody(IBodySource* source, Framing framing);

    /// No after UntilClose, Persistence::Close, an error, a close, a flow cancelled while it waited
    /// on the socket, or an unread body (a head read whose body has not reached its end, or a head
    /// written whose body has not been written), or, for a response role, bytes read past the end of
    /// the last response (they answer no request this side sent).
    [[nodiscard]] Reuse reusable() const noexcept;

    /// The socket, for its endpoints and TLS session. Requires a connection not moved from.
    [[nodiscard]] net::ISocket& socket() noexcept;

    /// Closes the socket; the connection is @c Reuse::No from now on.
    void close() noexcept;

  private:
    std::shared_ptr<detail::ConnectionState> _state;
};

} // namespace core::http
