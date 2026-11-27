// SPDX-License-Identifier: Apache-2.0
#include <core/http/Connection.hpp>

#include <core/Assert.hpp>
#include <core/http/detail/Syntax.hpp>
#include <core/net/DeadlineTimer.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketDeadline.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::http
{

namespace
{

    using namespace std::chrono_literals;

    // ---------------------------------------------------------------------------------------------
    // What differs between the two ends: one row per role, keyed by what the end reads.

    /// The codes of one @c MessageRole, named for what this end reads and what it writes.
    struct RoleRow
    {
        MessageRole reads;
        HttpErrorCode firstByteTimeout; ///< No byte of a head arrived within WireTimeouts::firstByte.
        HttpErrorCode incomplete;       ///< The peer closed inside a message.
        HttpErrorCode readBodySize;     ///< A body read is past HttpLimits::maxBodyBytes.
        HttpErrorCode writeBodySize;    ///< A body written breaks its Length framing.
    };

    constexpr auto RoleRows = std::array {
        RoleRow { .reads = MessageRole::Request,
                  .firstByteTimeout = HttpErrorCode::ConnectionReadTimeout,
                  .incomplete = HttpErrorCode::ProtocolError,
                  .readBodySize = HttpErrorCode::RequestBodySize,
                  .writeBodySize = HttpErrorCode::ResponseBodySize },
        RoleRow { .reads = MessageRole::Response,
                  .firstByteTimeout = HttpErrorCode::ResponseTimeout,
                  .incomplete = HttpErrorCode::ResponseIncomplete,
                  .readBodySize = HttpErrorCode::ResponseBodySize,
                  .writeBodySize = HttpErrorCode::RequestBodySize },
    };

    [[nodiscard]] RoleRow const& rowFor(MessageRole role)
    {
        auto const& row = RoleRows.at(static_cast<std::size_t>(role));
        Require(row.reads == role);
        return row;
    }

    // ---------------------------------------------------------------------------------------------
    // Socket failures, classified once: a row per NetErrorCode, a column per direction.

    /// Which way the failed operation moved bytes.
    enum class Direction : std::uint8_t
    {
        Read,
        Write,
    };

    struct NetErrorRow
    {
        net::NetErrorCode net;
        HttpErrorCode onRead;
        HttpErrorCode onWrite;
    };

    /// Every code not listed is @c HttpErrorCode::InternalError, with the socket's words as detail.
    constexpr auto NetErrorRows = std::array {
        NetErrorRow { .net = net::NetErrorCode::ConnReset,
                      .onRead = HttpErrorCode::ConnectionTerminated,
                      .onWrite = HttpErrorCode::ConnectionTerminated },
        NetErrorRow { .net = net::NetErrorCode::Eof,
                      .onRead = HttpErrorCode::ConnectionTerminated,
                      .onWrite = HttpErrorCode::ConnectionTerminated },
        // A cancel from the resource: the socket was closed or its read cancelled under the call.
        NetErrorRow { .net = net::NetErrorCode::Cancelled,
                      .onRead = HttpErrorCode::ConnectionTerminated,
                      .onWrite = HttpErrorCode::ConnectionTerminated },
        NetErrorRow { .net = net::NetErrorCode::BadHandle,
                      .onRead = HttpErrorCode::ConnectionTerminated,
                      .onWrite = HttpErrorCode::ConnectionTerminated },
        NetErrorRow { .net = net::NetErrorCode::Timeout,
                      .onRead = HttpErrorCode::ConnectionReadTimeout,
                      .onWrite = HttpErrorCode::ConnectionWriteTimeout },
        NetErrorRow { .net = net::NetErrorCode::ConnRefused,
                      .onRead = HttpErrorCode::ConnectionRefused,
                      .onWrite = HttpErrorCode::ConnectionRefused },
        NetErrorRow { .net = net::NetErrorCode::HostUnreach,
                      .onRead = HttpErrorCode::DestinationUnavailable,
                      .onWrite = HttpErrorCode::DestinationUnavailable },
    };

    [[nodiscard]] HttpError fromNet(net::NetError const& error, Direction direction)
    {
        auto const row = std::ranges::find(NetErrorRows, error.code, &NetErrorRow::net);
        auto code = HttpErrorCode::InternalError;
        if (row != NetErrorRows.end())
            code = direction == Direction::Read ? row->onRead : row->onWrite;
        return detail::refusal(code, error.toString());
    }

    // ---------------------------------------------------------------------------------------------
    // Heads as written: the framing fields the connection adds, as one row per framing.

    /// Which kind of message a head written is.
    enum class Written : std::uint8_t
    {
        Request,
        Response,
    };

    /// Whether a framing may be written in a context.
    enum class Allowed : std::uint8_t
    {
        No,
        Yes,
    };

    /// Whether bytes follow a head written with a framing.
    enum class Carries : std::uint8_t
    {
        NoBody,
        Body,
    };

    struct FramingWriteRow
    {
        BodyFraming kind;
        Allowed inRequest;      ///< UntilClose cannot end a request: its end is the response's start.
        Allowed inHttp10;       ///< HTTP/1.0 has no chunked coding.
        Carries carries;        ///< Whether the framing announces body bytes a status may forbid.
        std::string_view field; ///< The framing field; `{}` is the length. Empty for none.
    };

    constexpr auto FramingWriteRows = std::array {
        FramingWriteRow { .kind = BodyFraming::None,
                          .inRequest = Allowed::Yes,
                          .inHttp10 = Allowed::Yes,
                          .carries = Carries::NoBody,
                          .field = {} },
        FramingWriteRow { .kind = BodyFraming::Length,
                          .inRequest = Allowed::Yes,
                          .inHttp10 = Allowed::Yes,
                          .carries = Carries::Body,
                          .field = "Content-Length: {}\r\n" },
        FramingWriteRow { .kind = BodyFraming::Chunked,
                          .inRequest = Allowed::Yes,
                          .inHttp10 = Allowed::No,
                          .carries = Carries::Body,
                          .field = "Transfer-Encoding: chunked\r\n" },
        FramingWriteRow { .kind = BodyFraming::UntilClose,
                          .inRequest = Allowed::No,
                          .inHttp10 = Allowed::Yes,
                          .carries = Carries::Body,
                          .field = {} },
    };

    /// What a status lets a response's framing announce and send.
    enum class StatusBody : std::uint8_t
    {
        Permitted,  ///< Any framing, and the body it announces.
        LengthOnly, ///< A Content-Length may be announced, and no body follows (304, RFC 9110 8.6).
        Forbidden,  ///< No framing field and no body (1xx, 204; RFC 9110 8.6, RFC 9112 6.1).
    };

    struct StatusRow
    {
        std::uint16_t first;
        std::uint16_t last;
        StatusBody body;
    };

    /// The statuses that differ from @c StatusBody::Permitted.
    constexpr auto StatusRows = std::array {
        StatusRow { .first = 100, .last = 199, .body = StatusBody::Forbidden },
        StatusRow { .first = 204, .last = 204, .body = StatusBody::Forbidden },
        StatusRow { .first = 304, .last = 304, .body = StatusBody::LengthOnly },
    };

    [[nodiscard]] StatusBody statusBodyOf(std::uint16_t status)
    {
        auto const row = std::ranges::find_if(StatusRows, [status](StatusRow const& candidate) {
            return status >= candidate.first && status <= candidate.last;
        });
        return row == StatusRows.end() ? StatusBody::Permitted : row->body;
    }

    /// Whether a response's body is sent as its framing announces, or not at all (a HEAD's).
    enum class ReplyBody : std::uint8_t
    {
        AsFramed,
        Suppressed,
    };

    /// Whether body bytes follow a head written.
    enum class Sends : std::uint8_t
    {
        Nothing,    ///< The framing announces no body.
        Body,       ///< The body the framing announces follows.
        Suppressed, ///< The framing announces a body that is not sent (a HEAD's, a 304's).
    };

    /// A head's framing lines and whether a body follows them.
    struct HeadPlan
    {
        std::string lines;
        Sends sends;
    };

    [[nodiscard]] std::string_view versionText(HttpVersion version)
    {
        return version == HttpVersion::Http10 ? "HTTP/1.0" : "HTTP/1.1";
    }

    /// The framing fields a head of @p written gets for @p framing, and the head's empty line.
    /// @param status The response's status; ignored for a request.
    /// @param reply Whether the response's body is sent; ignored for a request.
    [[nodiscard]] HttpResult<HeadPlan> framingLines(
        Written written, HttpVersion version, std::uint16_t status, Framing framing, ReplyBody reply)
    {
        auto const& row = FramingWriteRows.at(static_cast<std::size_t>(framing.kind));
        Require(row.kind == framing.kind);
        if (written == Written::Request && row.inRequest == Allowed::No)
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, "a request cannot be framed until close"));
        if (version == HttpVersion::Http10 && row.inHttp10 == Allowed::No)
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError, "HTTP/1.0 has no chunked transfer coding"));
        auto const statusBody = written == Written::Request ? StatusBody::Permitted : statusBodyOf(status);
        auto const sendsBody = written == Written::Request || reply == ReplyBody::AsFramed;
        if (row.carries == Carries::Body && statusBody == StatusBody::Forbidden)
            return std::unexpected(detail::refusal(
                HttpErrorCode::ProtocolError, std::format("a {} response has no body to frame", status)));
        if (row.carries == Carries::Body && statusBody == StatusBody::LengthOnly
            && framing.kind != BodyFraming::Length)
            return std::unexpected(
                detail::refusal(HttpErrorCode::ProtocolError,
                                std::format("a {} response may announce only a length", status)));

        auto plan = HeadPlan { .lines = {}, .sends = Sends::Nothing };
        if (!row.field.empty())
            plan.lines += std::vformat(row.field, std::make_format_args(framing.length));
        // A response that may have a body and has none says so, or the peer reads it until close; a
        // response to HEAD announces nothing it was not given.
        if (framing.kind == BodyFraming::None && written == Written::Response
            && statusBody == StatusBody::Permitted && sendsBody)
            plan.lines += "Content-Length: 0\r\n";
        auto const closes =
            framing.persistence == Persistence::Close || framing.kind == BodyFraming::UntilClose;
        if (closes && version == HttpVersion::Http11)
            plan.lines += "Connection: close\r\n";
        if (!closes && version == HttpVersion::Http10)
            plan.lines += "Connection: keep-alive\r\n";
        plan.lines += "\r\n";
        auto const announcesBytes =
            row.carries == Carries::Body && (framing.kind != BodyFraming::Length || framing.length != 0);
        if (announcesBytes)
            plan.sends = statusBody == StatusBody::Permitted && sendsBody ? Sends::Body : Sends::Suppressed;
        return plan;
    }

    /// The caller's field lines, refusing any field the connection writes itself.
    [[nodiscard]] HttpResult<void> appendCallerFields(std::string& out, Fields const& fields)
    {
        for (auto const& field: fields.entries())
        {
            if (isFramingField(field.name))
                return std::unexpected(detail::refusal(HttpErrorCode::ProtocolError,
                                                       "the connection writes this framing field itself",
                                                       field.name));
            out += field.name;
            out += ": ";
            out += field.value;
            out += "\r\n";
        }
        return {};
    }

    [[nodiscard]] std::span<std::byte const> bytesOf(std::string_view text) noexcept
    {
        return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
    }

} // namespace

namespace
{

    /// Where the read side is between messages.
    enum class ReadPhase : std::uint8_t
    {
        Idle,     ///< Between messages: the last body, if any, reached its end.
        HeadRead, ///< A head was read; its body has no reader yet.
        InBody,   ///< A body is being read.
    };

    /// Where the write side is between messages.
    enum class WritePhase : std::uint8_t
    {
        Idle,        ///< Between messages.
        BodyPending, ///< A head announcing a body was written; the body has not been.
    };

    /// Whether another message may follow this exchange. Not a failure: a `Connection: close`
    /// exchange still runs to its end.
    enum class Persist : std::uint8_t
    {
        KeepAlive,    ///< Nothing has ruled out another message.
        LastExchange, ///< A framing or a field ends the connection after this exchange.
        Finished,     ///< That exchange's last message has been read: no head follows.
    };

    /// Whether one direction's byte stream is still where the protocol thinks it is. Kept per
    /// direction: a peer's malformed or stalled request breaks the read side, and the write side can
    /// still answer it (400, 408).
    enum class Stream : std::uint8_t
    {
        Sound,  ///< Every byte read or written is accounted for.
        Broken, ///< An error, a close, or a cancelled operation; nothing further moves on it.
    };

    /// Whether the request role still owes the response to the last request read.
    enum class Answer : std::uint8_t
    {
        Given,   ///< Every request read has its final response written in full.
        Pending, ///< The last request's final response is not yet written in full.
    };

    /// Whether one direction has a socket operation outstanding. Set before each await and cleared
    /// only when it resumes normally, so a flow cancelled (or destroyed) mid-await leaves it Busy: the
    /// message on that side is cut off at a byte nobody knows.
    enum class Wire : std::uint8_t
    {
        Idle,
        Busy,
    };

} // namespace

namespace detail
{

    /// The state a @c Connection and its body readers share.
    struct ConnectionState
    {
        net::EventLoop* loop;
        std::unique_ptr<net::ISocket> socket;
        MessageRole role;
        HttpLimits limits;
        WireTimeouts timeouts;
        /// The one read buffer, @c HttpLimits::maxReadChunk bytes; [begin, end) is unread.
        std::vector<std::byte> buffer;
        std::size_t begin = 0;
        std::size_t end = 0;
        ReadPhase readPhase = ReadPhase::Idle;
        WritePhase writePhase = WritePhase::Idle;
        Persist persist = Persist::KeepAlive;
        Stream readStream = Stream::Sound;
        Stream writeStream = Stream::Sound;
        Answer answer = Answer::Given;
        Wire readWire = Wire::Idle;
        Wire writeWire = Wire::Idle;
        /// Whether the next response written sends its body: not after a HEAD request.
        ReplyBody replyBody = ReplyBody::AsFramed;
        /// The framing the last head written announced, which writeBody must follow.
        Framing announced;
        /// Whether that head's body is sent, announced only (a HEAD's, a 304's), or none.
        Sends announcedSends = Sends::Nothing;
        /// Which head's body is current; a reader of an older one answers that its body is over.
        std::uint64_t generation = 0;
        /// The generation a live reader belongs to.
        std::optional<std::uint64_t> liveReader;
        Framing body;
        std::uint64_t remaining = 0;
        /// How much of an UntilClose body has been handed out, for maxBodyBytes.
        std::uint64_t untilCloseBytes = 0;
        std::optional<ChunkedDecoder> decoder;
        Fields trailers;
        /// The failure that ended the current body, repeated to every later read of it.
        std::optional<HttpError> bodyError;
        /// When the current body's whole-body bound (@c WireTimeouts::body) runs out, if it has one.
        std::optional<platform::SteadyTimePoint> bodyDeadline;

        ConnectionState(net::EventLoop* ownerLoop,
                        std::unique_ptr<net::ISocket> ownedSocket,
                        MessageRole readRole,
                        HttpLimits readLimits,
                        WireTimeouts readTimeouts):
            loop { ownerLoop },
            socket { std::move(ownedSocket) },
            role { readRole },
            limits { readLimits },
            timeouts { readTimeouts },
            buffer(readLimits.maxReadChunk)
        {
            Require(loop != nullptr && socket != nullptr);
            Require(!buffer.empty());
        }

        [[nodiscard]] std::string_view buffered() const noexcept
        {
            return { reinterpret_cast<char const*>(buffer.data()) + begin, end - begin };
        }

        void consume(std::size_t count) noexcept { begin += count; }

        /// Marks the read side broken, and hands @p error back.
        [[nodiscard]] HttpError fail(HttpError error) noexcept
        {
            readStream = Stream::Broken;
            return error;
        }

        /// Marks the write side broken, and hands @p error back.
        [[nodiscard]] HttpError failWrite(HttpError error) noexcept
        {
            writeStream = Stream::Broken;
            return error;
        }

        /// As @c fail, and every later read of the current body repeats @p error.
        [[nodiscard]] HttpError failBody(HttpError error)
        {
            bodyError = error;
            return fail(std::move(error));
        }

        /// Ends the current body cleanly, and with it a last exchange's read side.
        void reachEnd() noexcept
        {
            readPhase = ReadPhase::Idle;
            if (persist == Persist::LastExchange)
                persist = Persist::Finished;
        }
    };

} // namespace detail

namespace
{

    using detail::ConnectionState;

    /// Whether a read deadline has fired.
    enum class Expiry : std::uint8_t
    {
        Pending,
        Fired,
    };

    /// What a read deadline retires, and where it records that it did.
    struct ReadDeadline
    {
        net::ISocket* socket;
        Expiry expiry = Expiry::Pending;
    };

    /// Reads into the empty buffer, bounded by @p timeout unless it is zero (R31).
    ///
    /// The deadline is a @c net::DeadlineTimer that retires the parked read through
    /// @c ISocket::cancelRead rather than a @c net::withTimeout race, for two reasons. The socket
    /// stays open, so a server whose head read timed out can still answer 408 on it; and the read
    /// answers the cancel as a VALUE (@c NetErrorCode::Cancelled), so the timeout is reported where
    /// the read returns rather than as an @c OperationCancelled unwinding the caller's flow. The
    /// timer callback runs on the loop's thread, where @c cancelRead must be called. A read that
    /// settles with bytes despite the cancel -- a completion transport's whose receive had already
    /// finished -- keeps them: the data wins. A socket decorator that does not forward
    /// @c cancelRead leaves the deadline with nothing to retire, and the read unbounded.
    ///
    /// **The read side's one guard against a cut stream.** Every head and body read reaches the
    /// socket through here, and a read side found broken, or busy from a read a cancel left behind,
    /// is refused before any byte moves; @c send is the write side's.
    /// @return The byte count, zero for the peer's EOF, or the failure (which breaks the stream).
    async::Task<HttpResult<std::size_t>> fill(std::shared_ptr<ConnectionState> state,
                                              std::chrono::milliseconds timeout,
                                              HttpErrorCode timeoutCode,
                                              RefusalReason timeoutReason = RefusalReason::Unclassified)
    {
        if (state->readWire == Wire::Busy || state->readStream == Stream::Broken)
            co_return std::unexpected(state->fail(detail::refusal(
                HttpErrorCode::ConnectionTerminated, "a cancelled or failed read cut the stream off")));
        Require(state->begin == state->end);
        state->begin = 0;
        state->end = 0;
        auto deadline = ReadDeadline { .socket = state->socket.get(), .expiry = Expiry::Pending };
        auto timer = std::optional<net::DeadlineTimer> {};
        if (timeout > 0ms)
            timer.emplace(
                *state->loop,
                state->loop->clock().now() + timeout,
                [](void* target) {
                    auto& fired = *static_cast<ReadDeadline*>(target);
                    fired.expiry = Expiry::Fired; // before the cancel, which may resume the reader
                    fired.socket->cancelRead();
                },
                &deadline);
        state->readWire = Wire::Busy;
        auto const result = co_await state->socket->read(std::span<std::byte> { state->buffer });
        state->readWire = Wire::Idle;
        timer.reset();
        if (!result.has_value() && deadline.expiry == Expiry::Fired)
            co_return std::unexpected(
                state->fail(detail::refusal(timeoutCode,
                                            std::format("no byte arrived within {} ms", timeout.count()),
                                            std::nullopt,
                                            std::nullopt,
                                            timeoutReason)));
        if (!result.has_value())
            co_return std::unexpected(state->fail(fromNet(result.error(), Direction::Read)));
        state->end = *result;
        co_return *result;
    }

    /// Whether a head read has seen any of its bytes yet.
    enum class HeadProgress : std::uint8_t
    {
        Nothing,
        Started,
    };

    /// Reads one head through a @c HeadParser, from the buffer first, then the socket.
    async::Task<HttpResult<HeadParser>> readHead(std::shared_ptr<ConnectionState> state)
    {
        if (state->readStream == Stream::Broken || state->readWire == Wire::Busy)
            co_return std::unexpected(
                state->fail(detail::refusal(HttpErrorCode::ConnectionTerminated,
                                            "the stream is broken, or a cancelled read cut it off")));
        if (state->persist == Persist::Finished)
            co_return std::unexpected(detail::refusal(HttpErrorCode::ConnectionTerminated,
                                                      "the connection's last exchange is over"));
        if (state->answer == Answer::Pending)
            co_return std::unexpected(detail::refusal(
                HttpErrorCode::ProtocolError,
                "the request role is serial: the last request's response is not yet written"));
        Require(state->readPhase == ReadPhase::Idle);
        auto const& row = rowFor(state->role);
        auto parser = HeadParser { state->role, state->limits };
        auto progress = HeadProgress::Nothing;
        // The whole head's bound starts at its first byte: the wait for that byte is firstByte's
        // budget alone, so a client that idles is not charged for it twice.
        auto headDeadline = std::optional<platform::SteadyTimePoint> {};
        while (true)
        {
            if (auto const bytes = state->buffered(); !bytes.empty())
            {
                if (progress == HeadProgress::Nothing && state->timeouts.head > 0ms)
                    headDeadline = state->loop->clock().now() + state->timeouts.head;
                progress = HeadProgress::Started;
                auto fed = parser.feed(bytes);
                if (!fed.has_value())
                    co_return std::unexpected(state->fail(std::move(fed.error())));
                state->consume(fed->consumed);
                if (fed->progress == Progress::Complete)
                {
                    state->readPhase = ReadPhase::HeadRead;
                    ++state->generation;
                    state->liveReader.reset();
                    state->bodyError.reset();
                    state->trailers = Fields {};
                    co_return parser;
                }
                continue;
            }
            auto const first = progress == HeadProgress::Nothing;
            auto const nothing = first ? RefusalReason::NothingArrived : RefusalReason::Unclassified;
            auto wait = first ? state->timeouts.firstByte : state->timeouts.betweenBytes;
            auto code = first ? row.firstByteTimeout : HttpErrorCode::ConnectionReadTimeout;
            if (headDeadline.has_value())
            {
                // The whole head's bound, asked before every read rather than armed once: a read
                // whose bytes beat its cancel leaves no timer behind to bound the next one.
                auto const now = state->loop->clock().now();
                if (now >= *headDeadline)
                    co_return std::unexpected(state->fail(detail::refusal(
                        HttpErrorCode::ConnectionReadTimeout,
                        std::format("the head took longer than {} ms", state->timeouts.head.count()))));
                auto const left = std::chrono::ceil<std::chrono::milliseconds>(*headDeadline - now);
                if (wait <= 0ms || left < wait)
                {
                    wait = left;
                    code = HttpErrorCode::ConnectionReadTimeout;
                }
            }
            auto got = co_await fill(state, wait, code, nothing);
            if (!got.has_value())
                co_return std::unexpected(std::move(got.error()));
            if (*got == 0)
                co_return std::unexpected(
                    state->fail(first ? detail::refusal(HttpErrorCode::ConnectionTerminated,
                                                        "the peer closed before a head")
                                      : detail::refusal(row.incomplete, "the peer closed inside a head")));
        }
    }

    async::Task<HttpResult<RequestHead>> readRequest(std::shared_ptr<ConnectionState> state)
    {
        auto parser = co_await readHead(state);
        if (!parser.has_value())
            co_return std::unexpected(std::move(parser.error()));
        auto head = parser->takeRequest();
        state->replyBody = head.method.text() == "HEAD" ? ReplyBody::Suppressed : ReplyBody::AsFramed;
        state->answer = Answer::Pending;
        co_return head;
    }

    async::Task<HttpResult<ResponseHead>> readResponse(std::shared_ptr<ConnectionState> state)
    {
        auto parser = co_await readHead(state);
        if (!parser.has_value())
            co_return std::unexpected(std::move(parser.error()));
        auto head = parser->takeResponse();
        // An interim response has no body, whatever its fields say; the final one follows it.
        if (head.status < 200)
            state->readPhase = ReadPhase::Idle;
        co_return head;
    }

    /// Copies @p bytes out of the buffer into a chunk the caller owns.
    [[nodiscard]] BodyChunk chunkOf(std::string_view bytes, BodyEnd end)
    {
        auto chunk = BodyChunk { .bytes = std::vector<std::byte>(bytes.size()), .end = end };
        std::ranges::transform(
            bytes, chunk.bytes.begin(), [](char octet) { return static_cast<std::byte>(octet); });
        return chunk;
    }

    /// The deadline of the next body read: @c WireTimeouts::betweenBytes, capped by what is left of
    /// the whole body's bound; or the refusal once that bound has run out.
    [[nodiscard]] HttpResult<std::chrono::milliseconds> bodyWait(ConnectionState const& state)
    {
        auto const wait = state.timeouts.betweenBytes;
        if (!state.bodyDeadline.has_value())
            return wait;
        auto const now = state.loop->clock().now();
        if (now >= *state.bodyDeadline)
            return std::unexpected(
                detail::refusal(HttpErrorCode::ConnectionReadTimeout,
                                std::format("the body took longer than {} ms", state.timeouts.body.count())));
        auto const left = std::chrono::ceil<std::chrono::milliseconds>(*state.bodyDeadline - now);
        return wait <= 0ms || left < wait ? left : wait;
    }

    /// Reads more of a body into the empty buffer; an EOF is the role's incomplete-message code.
    async::Task<HttpResult<void>> fillBody(std::shared_ptr<ConnectionState> state)
    {
        auto const wait = bodyWait(*state);
        if (!wait.has_value())
            co_return std::unexpected(state->failBody(wait.error()));
        auto got = co_await fill(state, *wait, HttpErrorCode::ConnectionReadTimeout);
        if (!got.has_value())
            co_return std::unexpected(state->failBody(std::move(got.error())));
        if (*got == 0)
            co_return std::unexpected(state->failBody(
                detail::refusal(rowFor(state->role).incomplete, "the peer closed inside a body")));
        co_return {};
    }

    async::Task<HttpResult<BodyChunk>> readLength(std::shared_ptr<ConnectionState> state, std::size_t want)
    {
        if (state->buffered().empty())
            if (auto filled = co_await fillBody(state); !filled.has_value())
                co_return std::unexpected(std::move(filled.error()));
        auto const take = static_cast<std::size_t>(
            std::min<std::uint64_t>({ want, state->remaining, state->buffered().size() }));
        auto const bytes = state->buffered().substr(0, take);
        state->consume(take);
        state->remaining -= take;
        if (state->remaining == 0)
            state->reachEnd();
        co_return chunkOf(bytes, state->remaining == 0 ? BodyEnd::Reached : BodyEnd::More);
    }

    async::Task<HttpResult<BodyChunk>> readUntilClose(std::shared_ptr<ConnectionState> state,
                                                      std::size_t want)
    {
        if (state->buffered().empty())
        {
            auto const wait = bodyWait(*state);
            if (!wait.has_value())
                co_return std::unexpected(state->failBody(wait.error()));
            auto got = co_await fill(state, *wait, HttpErrorCode::ConnectionReadTimeout);
            if (!got.has_value())
                co_return std::unexpected(state->failBody(std::move(got.error())));
            if (*got == 0)
            {
                state->reachEnd();
                co_return BodyChunk { .bytes = {}, .end = BodyEnd::Reached };
            }
        }
        auto const bytes = state->buffered().substr(0, want);
        if (auto const limit = state->limits.maxBodyBytes;
            limit.has_value() && state->untilCloseBytes + bytes.size() > *limit)
            co_return std::unexpected(
                state->failBody(detail::refusal(rowFor(state->role).readBodySize,
                                                "the body is past maxBodyBytes",
                                                std::nullopt,
                                                state->untilCloseBytes + bytes.size())));
        state->untilCloseBytes += bytes.size();
        state->consume(bytes.size());
        co_return chunkOf(bytes, BodyEnd::More);
    }

    async::Task<HttpResult<BodyChunk>> readChunked(std::shared_ptr<ConnectionState> state, std::size_t want)
    {
        while (true)
        {
            if (auto const bytes = state->buffered(); !bytes.empty())
            {
                // At most `want` bytes go in, so no more than that comes out as data.
                auto step = state->decoder->feed(bytes.substr(0, want));
                if (!step.has_value())
                    co_return std::unexpected(state->failBody(std::move(step.error())));
                auto chunk = chunkOf(step->data, BodyEnd::More);
                state->consume(step->consumed);
                if (!chunk.bytes.empty())
                    co_return chunk;
                if (step->progress == Progress::Complete)
                {
                    state->trailers = state->decoder->takeTrailers();
                    state->reachEnd();
                    co_return BodyChunk { .bytes = {}, .end = BodyEnd::Reached };
                }
                continue;
            }
            if (auto filled = co_await fillBody(state); !filled.has_value())
                co_return std::unexpected(std::move(filled.error()));
        }
    }

    /// A body of the connection: a view of its shared state, for one head's body.
    class BodyReader final: public IBodyReader
    {
      public:
        BodyReader(std::shared_ptr<ConnectionState> state, std::uint64_t generation):
            _state { std::move(state) }, _generation { generation }
        {
            _state->liveReader = _generation;
        }

        ~BodyReader() override
        {
            if (_state->liveReader == _generation)
                _state->liveReader.reset();
        }

        BodyReader(BodyReader const&) = delete;
        BodyReader& operator=(BodyReader const&) = delete;
        BodyReader(BodyReader&&) = delete;
        BodyReader& operator=(BodyReader&&) = delete;

        [[nodiscard]] async::Task<HttpResult<BodyChunk>> read(std::size_t maxBytes) override
        {
            return readBody(_state, _generation, maxBytes);
        }

        [[nodiscard]] async::Task<HttpResult<Fields>> trailers() override
        {
            return trailersOf(_state, _generation);
        }

      private:
        [[nodiscard]] static async::Task<HttpResult<BodyChunk>> readBody(
            std::shared_ptr<ConnectionState> state, std::uint64_t generation, std::size_t maxBytes)
        {
            if (generation != state->generation)
                co_return BodyChunk { .bytes = {}, .end = BodyEnd::Reached };
            if (state->bodyError.has_value())
                co_return std::unexpected(*state->bodyError);
            if (state->readPhase != ReadPhase::InBody)
                co_return BodyChunk { .bytes = {}, .end = BodyEnd::Reached };
            // No explicit clamp to maxReadChunk: every framing hands out at most what the buffer
            // holds, and the buffer is maxReadChunk bytes, so that is the bound (D3).
            auto const want = maxBytes;
            if (want == 0)
                co_return BodyChunk { .bytes = {}, .end = BodyEnd::More };
            switch (state->body.kind)
            {
                case BodyFraming::Length: co_return co_await readLength(state, want);
                case BodyFraming::Chunked: co_return co_await readChunked(state, want);
                case BodyFraming::UntilClose: co_return co_await readUntilClose(state, want);
                case BodyFraming::None: break; // never in ReadPhase::InBody: bodyReader ends it at once
            }
            co_return BodyChunk { .bytes = {}, .end = BodyEnd::Reached };
        }

        [[nodiscard]] static async::Task<HttpResult<Fields>> trailersOf(
            std::shared_ptr<ConnectionState> state, std::uint64_t generation)
        {
            if (generation != state->generation)
                co_return Fields {};
            if (state->bodyError.has_value())
                co_return std::unexpected(*state->bodyError);
            Require(state->readPhase != ReadPhase::InBody); // trailers() before BodyEnd::Reached
            co_return std::move(state->trailers);
        }

        std::shared_ptr<ConnectionState> _state;
        std::uint64_t _generation;
    };

    /// Writes @p segments in full, as one gather write, bounded by WireTimeouts::write unless it is
    /// zero. The deadline closes the socket (@c net::armSocketDeadline): a write cut off mid-message
    /// leaves a stream nothing can resynchronise. Like @c fill for the read side, it is the write side's
    /// one guard: every head and body write passes it, and a write side found broken, or busy from a
    /// write a cancel left behind, is refused before any byte moves.
    async::Task<HttpResult<void>> send(std::shared_ptr<ConnectionState> state,
                                       std::span<std::span<std::byte const> const> segments)
    {
        if (state->writeWire == Wire::Busy || state->writeStream == Stream::Broken)
            co_return std::unexpected(state->failWrite(detail::refusal(
                HttpErrorCode::ConnectionTerminated, "a cancelled or failed write cut the stream off")));
        auto target = net::SocketDeadlineTarget { .socket = state->socket.get(), .expired = false };
        auto const deadline = net::armSocketDeadline(state->loop, state->timeouts.write, &target);
        state->writeWire = Wire::Busy;
        auto const wrote = co_await state->socket->writeVectored(segments);
        state->writeWire = Wire::Idle;
        if (!wrote.has_value() && target.expired)
            co_return std::unexpected(state->failWrite(detail::refusal(
                HttpErrorCode::ConnectionWriteTimeout,
                std::format("a write did not finish within {} ms", state->timeouts.write.count()))));
        if (!wrote.has_value())
            co_return std::unexpected(state->failWrite(fromNet(wrote.error(), Direction::Write)));
        co_return {};
    }

    /// Writes one head: @p startLine, the caller's fields, and the framing fields, in one write.
    async::Task<HttpResult<void>> writeHead(std::shared_ptr<ConnectionState> state,
                                            Written written,
                                            std::string startLine,
                                            Fields fields,
                                            HttpVersion version,
                                            std::uint16_t status,
                                            Framing framing)
    {
        Require(state->writePhase == WritePhase::Idle); // the last head's body is written first
        auto head = std::move(startLine);
        // Refused before the first wire byte: the connection is left as it was, so the caller may
        // send a corrected head (a handler's 500 after its own bad one). Nothing above changes state.
        // A status line carries three digits: anything else would be a malformed line on the wire.
        if (written == Written::Response && (status < 100 || status > 999))
            co_return std::unexpected(detail::refusal(HttpErrorCode::ProtocolError,
                                                      std::format("status {} is not three digits", status)));
        if (auto appended = appendCallerFields(head, fields); !appended.has_value())
            co_return std::unexpected(std::move(appended.error()));
        auto const reply = written == Written::Response ? state->replyBody : ReplyBody::AsFramed;
        auto plan = framingLines(written, version, status, framing, reply);
        if (!plan.has_value())
            co_return std::unexpected(std::move(plan.error()));
        auto const segments = std::array { bytesOf(head), bytesOf(plan->lines) };
        if (auto sent = co_await send(state, segments); !sent.has_value())
            co_return sent;
        auto const answers = written == Written::Response && status >= 200;
        if (answers)
            state->replyBody = ReplyBody::AsFramed; // the HEAD request is answered; a 1xx does not answer it
        state->announced = framing;
        state->announcedSends = plan->sends;
        state->writePhase = plan->sends == Sends::Body ? WritePhase::BodyPending : WritePhase::Idle;
        if (answers && state->writePhase == WritePhase::Idle)
            state->answer = Answer::Given;
        if ((framing.persistence == Persistence::Close || framing.kind == BodyFraming::UntilClose)
            && state->persist == Persist::KeepAlive) // never undoes Finished
            state->persist = Persist::LastExchange;
        co_return {};
    }

    /// Whether a framing holds the body to an exact count, and to which.
    [[nodiscard]] std::optional<std::uint64_t> exactLength(Framing framing)
    {
        if (framing.kind == BodyFraming::None)
            return std::uint64_t { 0 };
        if (framing.kind == BodyFraming::Length)
            return framing.length;
        return std::nullopt;
    }

    async::Task<HttpResult<void>> writeBodyOf(std::shared_ptr<ConnectionState> state,
                                              IBodySource* source,
                                              Framing framing)
    {
        auto const announced = std::exchange(state->announced, Framing {});
        auto const sends = std::exchange(state->announcedSends, Sends::Nothing);
        if (framing.kind != announced.kind || framing.length != announced.length)
            co_return std::unexpected(state->failWrite(detail::refusal(
                HttpErrorCode::ProtocolError, "the body's framing is not the one its head announced")));
        if (sends == Sends::Suppressed)
        {
            // Announced only: a response to HEAD, or a 304. Nothing follows the head.
            state->writePhase = WritePhase::Idle;
            state->answer = Answer::Given;
            co_return {};
        }
        auto const sizeCode = rowFor(state->role).writeBodySize;
        auto const exact = exactLength(framing);
        auto const intent = source != nullptr ? source->trailerIntent() : TrailerIntent::None;
        if (intent == TrailerIntent::Possible && framing.kind != BodyFraming::Chunked)
            co_return std::unexpected(state->failWrite(
                detail::refusal(HttpErrorCode::ProtocolError, "trailers need the chunked framing")));
        if (auto const known = source != nullptr ? source->knownLength() : std::optional<std::uint64_t> { 0 };
            exact.has_value() && known.has_value() && *known != *exact)
            co_return std::unexpected(state->failWrite(
                detail::refusal(sizeCode,
                                std::format("a body of {} bytes under a length of {}", *known, *exact),
                                std::nullopt,
                                known)));

        auto written = std::uint64_t { 0 };
        auto end = source != nullptr ? BodyEnd::More : BodyEnd::Reached;
        while (end == BodyEnd::More)
        {
            auto chunk = co_await source->next();
            if (!chunk.has_value())
                co_return std::unexpected(state->failWrite(std::move(chunk.error())));
            end = chunk->end;
            auto const bytes = std::span<std::byte const> { chunk->bytes };
            if (exact.has_value() && written + bytes.size() > *exact)
                co_return std::unexpected(state->failWrite(
                    detail::refusal(sizeCode,
                                    std::format("the body runs past its length of {}", *exact),
                                    std::nullopt,
                                    written + bytes.size())));
            if (bytes.empty())
                continue;
            written += bytes.size();
            if (framing.kind == BodyFraming::Chunked)
            {
                auto const header = chunkHeader(bytes.size());
                auto const segments = std::array { bytesOf(header), bytes, bytesOf("\r\n") };
                if (auto sent = co_await send(state, segments); !sent.has_value())
                    co_return sent;
            }
            else
            {
                auto const segments = std::array { bytes };
                if (auto sent = co_await send(state, segments); !sent.has_value())
                    co_return sent;
            }
        }
        if (exact.has_value() && written != *exact)
            co_return std::unexpected(state->failWrite(
                detail::refusal(sizeCode,
                                std::format("the body ends short of its length of {}", *exact),
                                std::nullopt,
                                written)));

        if (framing.kind == BodyFraming::Chunked)
        {
            auto trailers = Fields {};
            if (intent == TrailerIntent::Possible)
            {
                auto given = co_await source->trailers();
                if (!given.has_value())
                    co_return std::unexpected(state->failWrite(std::move(given.error())));
                trailers = std::move(*given);
            }
            for (auto const& field: trailers.entries())
                if (isFramingField(field.name))
                    co_return std::unexpected(state->failWrite(detail::refusal(
                        HttpErrorCode::ProtocolError, "a trailer cannot be a framing field", field.name)));
            auto const last = lastChunk(trailers);
            auto const segments = std::array { bytesOf(last) };
            if (auto sent = co_await send(state, segments); !sent.has_value())
                co_return sent;
        }
        state->writePhase = WritePhase::Idle;
        state->answer = Answer::Given; // a body written is a final response's: a 1xx has none
        co_return {};
    }

} // namespace

Connection::Connection(net::EventLoop& loop,
                       std::unique_ptr<net::ISocket> socket,
                       MessageRole role,
                       HttpLimits limits,
                       WireTimeouts timeouts):
    _state { std::make_shared<ConnectionState>(&loop, std::move(socket), role, limits, timeouts) }
{
}

Connection::~Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

async::Task<HttpResult<RequestHead>> Connection::readRequestHead()
{
    Require(_state != nullptr && _state->role == MessageRole::Request);
    return readRequest(_state);
}

async::Task<HttpResult<ResponseHead>> Connection::readResponseHead()
{
    Require(_state != nullptr && _state->role == MessageRole::Response);
    return readResponse(_state);
}

std::unique_ptr<IBodyReader> Connection::bodyReader(Framing framing)
{
    Require(_state != nullptr && !hasLiveBodyReader());
    auto& state = *_state;
    if (state.readPhase == ReadPhase::HeadRead)
    {
        state.body = framing;
        state.remaining = framing.kind == BodyFraming::Length ? framing.length : 0;
        // Emplaced or reset, never reset and then emplaced: GCC 15 reads the reset that emplace
        // makes again as a destruction of fields it calls maybe uninitialized.
        if (framing.kind == BodyFraming::Chunked)
            state.decoder.emplace(state.role, state.limits);
        else
            state.decoder.reset();
        state.untilCloseBytes = 0;
        state.bodyDeadline.reset();
        if (state.timeouts.body > 0ms)
            state.bodyDeadline = state.loop->clock().now() + state.timeouts.body;
        if ((framing.persistence == Persistence::Close || framing.kind == BodyFraming::UntilClose)
            && state.persist == Persist::KeepAlive)
            state.persist = Persist::LastExchange;
        auto const empty =
            framing.kind == BodyFraming::None || (framing.kind == BodyFraming::Length && framing.length == 0);
        state.readPhase = ReadPhase::InBody;
        if (empty)
            state.reachEnd();
        if (framing.kind == BodyFraming::Length && state.limits.maxBodyBytes.has_value()
            && framing.length > *state.limits.maxBodyBytes)
            std::ignore = state.failBody(detail::refusal(rowFor(state.role).readBodySize,
                                                         "the declared length is past maxBodyBytes",
                                                         std::nullopt,
                                                         framing.length));
    }
    else if (state.readPhase == ReadPhase::InBody)
        Require(framing.kind == state.body.kind && framing.length == state.body.length); // the same body
    return std::make_unique<BodyReader>(_state, state.generation);
}

bool Connection::hasLiveBodyReader() const noexcept
{
    return _state != nullptr && _state->liveReader == _state->generation;
}

async::Task<HttpResult<void>> Connection::writeRequestHead(RequestHead head, Framing framing)
{
    Require(_state != nullptr && _state->role == MessageRole::Response);
    auto startLine =
        std::format("{} {} {}\r\n", head.method.text(), head.target.text(), versionText(head.version));
    return writeHead(
        _state, Written::Request, std::move(startLine), std::move(head.fields), head.version, 0, framing);
}

async::Task<HttpResult<void>> Connection::writeResponseHead(ResponseHead head, Framing framing)
{
    Require(_state != nullptr && _state->role == MessageRole::Request);
    auto startLine = std::format("{} {} {}\r\n", versionText(head.version), head.status, head.reason.text());
    return writeHead(_state,
                     Written::Response,
                     std::move(startLine),
                     std::move(head.fields),
                     head.version,
                     head.status,
                     framing);
}

async::Task<HttpResult<void>> Connection::writeBody(IBodySource* source, Framing framing)
{
    Require(_state != nullptr);
    return writeBodyOf(_state, source, framing);
}

Reuse Connection::reusable() const noexcept
{
    if (_state == nullptr)
        return Reuse::No;
    auto const& state = *_state;
    auto const clean = state.readStream == Stream::Sound && state.writeStream == Stream::Sound
                       && state.persist == Persist::KeepAlive && !state.socket->isClosed()
                       && state.readPhase == ReadPhase::Idle && state.writePhase == WritePhase::Idle
                       && state.readWire == Wire::Idle && state.writeWire == Wire::Idle;
    // Bytes a response role holds past the end of its last response answer no request it sent: pooled,
    // they would be read as the next request's response. A request role keeps them, as the start of the
    // next pipelined request.
    auto const answered = state.role == MessageRole::Request || state.begin == state.end;
    return clean && answered ? Reuse::Yes : Reuse::No;
}

net::ISocket& Connection::socket() noexcept
{
    Require(_state != nullptr);
    return *_state->socket;
}

void Connection::close() noexcept
{
    if (_state == nullptr)
        return;
    _state->readStream = Stream::Broken;
    _state->writeStream = Stream::Broken;
    _state->socket->close();
}

} // namespace core::http
