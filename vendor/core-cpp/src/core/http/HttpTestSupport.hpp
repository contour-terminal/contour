// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only helpers the `core::http` cases share: raw wire text written to a socket, a body read
/// to its end, a flow run on a hand-driven loop until it has an answer, a server end a case scripts,
/// a served connection a case drives byte by byte with the handlers it serves, and a TLS context whose
/// handshake and failures a case scripts (R29: no `core::net_tls`, so the binaries build without
/// `CORE_CPP_WITH_TLS`). Test-only: in no `FILE_SET`, so never installed.

#include <core/async/Task.hpp>
#include <core/http/Body.hpp>
#include <core/http/Connection.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpClient.hpp>
#include <core/http/HttpServer.hpp>
#include <core/http/Message.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsContext.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/NetError.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::http::test
{

/// Whether this process runs under wine rather than on Windows or a POSIX system. Defined per
/// platform (`posix/HttpTestSupport.cpp`, `windows/HttpTestSupport.cpp`); only a binary that links
/// one of them may call it. A case uses it to accept a behaviour wine has and no native kernel
/// does, and to fail where the same behaviour shows natively. A @c bool because the name asks it.
[[nodiscard]] bool runsUnderWine() noexcept;

/// What a loopback connection does with a flood its peer never reads.
enum class LoopbackFlood : std::uint8_t
{
    Stalls,   ///< The writer stalls once the kernel buffers are full (Linux, macOS, the BSDs).
    Absorbed, ///< The whole flood is taken: Windows, natively and under wine, which tunes a
              ///< loopback window past the buffer sizes it was given (measured on CI with 4 and
              ///< 64 MiB).
};

/// @return What this platform's loopback does with an unread flood; defined beside
///         @c runsUnderWine.
[[nodiscard]] LoopbackFlood loopbackFlood() noexcept;

/// @param text The bytes to view.
/// @return @p text as the byte span a socket write takes.
[[nodiscard]] inline std::span<std::byte const> bytesOf(std::string_view text) noexcept
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// @param bytes Bytes a socket or a body reader handed out.
/// @return The same bytes as text.
[[nodiscard]] inline std::string textOf(std::span<std::byte const> bytes)
{
    return { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
}

/// Writes @p text to @p socket in full, then closes it: a peer that sends exactly these bytes.
/// @param socket The writer; must outlive the returned task.
/// @param text The wire bytes, owned by the frame.
inline async::Task<void> writeAndClose(net::ISocket* socket, std::string text)
{
    if (!text.empty())
        static_cast<void>(co_await socket->write(bytesOf(text)));
    socket->close();
}

/// Writes @p text to @p socket in full and leaves it open: a peer that then falls silent.
/// @param socket The writer; must outlive the returned task.
/// @param text The wire bytes, owned by the frame.
inline async::Task<void> writeOnly(net::ISocket* socket, std::string text)
{
    static_cast<void>(co_await socket->write(bytesOf(text)));
}

/// Reads from @p socket until EOF or an error: what a peer received.
/// @param socket The reader; must outlive the returned task.
/// @return Every byte read, in order.
inline async::Task<std::string> readUntilClosed(net::ISocket* socket)
{
    auto text = std::string {};
    auto chunk = std::vector<std::byte>(256);
    while (true)
    {
        auto const got = co_await socket->read(std::span<std::byte> { chunk });
        if (!got.has_value() || *got == 0)
            co_return text;
        text += textOf(std::span<std::byte const> { chunk }.first(*got));
    }
}

/// Reads one bodiless request head off @p socket as raw bytes and answers it with @p reply: a server
/// end that takes a second request on a connection a @c ScriptedServer, whose @c Connection holds its
/// raw reply for unwritten, has already served.
/// @param socket The server end; must outlive the returned task.
/// @param reply The raw response bytes.
/// @return The request's start line, or empty if the peer closed or failed first.
inline async::Task<std::string> answerHeadOnly(net::ISocket* socket, std::string reply)
{
    auto text = std::string {};
    auto chunk = std::vector<std::byte>(512);
    while (!text.contains("\r\n\r\n"))
    {
        auto const got = co_await socket->read(std::span<std::byte> { chunk });
        if (!got.has_value() || *got == 0)
            co_return std::string {};
        text += textOf(std::span<std::byte const> { chunk }.first(*got));
    }
    static_cast<void>(co_await socket->write(bytesOf(reply)));
    co_return text.substr(0, text.find("\r\n"));
}

/// A body read to its end, chunk by chunk.
struct ReadBody
{
    std::string text;                    ///< Every body byte, in order.
    std::vector<std::size_t> chunkSizes; ///< The size of each chunk @c read handed out.
    std::optional<HttpError> error;      ///< What ended the read early, if anything did.
};

/// Reads @p reader until @c BodyEnd::Reached or an error, @p maxBytes at a time.
/// @param reader The body; must outlive the returned task.
/// @param maxBytes What each @c read asks for.
/// @return What was read and what, if anything, ended it.
inline async::Task<ReadBody> readAll(IBodyReader* reader, std::size_t maxBytes)
{
    auto out = ReadBody {};
    while (true)
    {
        auto chunk = co_await reader->read(maxBytes);
        if (!chunk.has_value())
        {
            out.error = std::move(chunk.error());
            co_return out;
        }
        out.chunkSizes.push_back(chunk->bytes.size());
        out.text += textOf(chunk->bytes);
        if (chunk->end == BodyEnd::Reached)
            co_return out;
    }
}

/// Runs @p task and stores its answer in @p out, for a case that drives the loop by hand and then
/// asserts the answer is there: a flow that never finishes leaves @p out empty, never a hang.
/// @tparam T The task's result.
/// @param task The flow.
/// @param out Where its answer goes; must outlive the returned task.
template <typename T>
async::Task<void> into(async::Task<T> task, std::optional<T>* out)
{
    *out = co_await std::move(task);
}

// -------------------------------------------------------------------------------------------------
// The server end of a scripted exchange.

/// What a server end read of one request: the head and the body.
struct ServedRequest
{
    std::optional<RequestHead> head; ///< The head, if it was read.
    ReadBody body;                   ///< The body, as far as it was read.
    std::optional<HttpError> error;  ///< What ended the read early, if anything did.
};

/// Whether a scripted server closes its end after its reply.
enum class AfterReply : std::uint8_t
{
    StaysOpen,
    Closes,
};

/// The server end of one connection a case scripts: it reads a request through a @c Connection in
/// the request role, and answers with raw bytes, so a case can send what no well-behaved server would.
class ScriptedServer
{
  public:
    /// @param loop The loop the connection runs on.
    /// @param socket The server end, from @c testing::ScriptedConnector::acceptNext; must not be null.
    ScriptedServer(net::EventLoop& loop, std::unique_ptr<net::ISocket> socket):
        _connection { loop, std::move(socket), MessageRole::Request, HttpLimits {}, WireTimeouts {} }
    {
    }

    /// @return The connection, for its socket.
    [[nodiscard]] Connection& connection() noexcept { return _connection; }

    /// Reads one request, its head and its body to the end, then writes @p reply as it is.
    /// @param reply The raw response bytes; none writes nothing.
    /// @param after Whether to close once they are written.
    /// @return What was read.
    [[nodiscard]] async::Task<ServedRequest> answer(std::string reply, AfterReply after)
    {
        return answerOn(&_connection, std::move(reply), after);
    }

  private:
    static async::Task<ServedRequest> answerOn(Connection* connection, std::string reply, AfterReply after)
    {
        auto out = ServedRequest {};
        auto head = co_await connection->readRequestHead();
        if (!head.has_value())
        {
            out.error = std::move(head.error());
            co_return out;
        }
        auto framing = requestFraming(head->version, head->fields);
        if (!framing.has_value())
        {
            out.error = std::move(framing.error());
            co_return out;
        }
        out.head = std::move(*head);
        auto reader = connection->bodyReader(*framing);
        out.body = co_await readAll(reader.get(), 4096);
        if (!reply.empty())
            static_cast<void>(co_await connection->socket().write(bytesOf(reply)));
        if (after == AfterReply::Closes)
            connection->close();
        co_return out;
    }

    Connection _connection;
};

/// A request for @p url, `http://host[:port]/target` or `https://…`, IPv6 hosts in brackets; without
/// a port the origin names none, so the scheme's default applies.
/// @param url The URL; anything else fails the case.
/// @param method The method.
/// @return The request, without a body.
[[nodiscard]] inline OutgoingRequest get(std::string_view url, std::string_view method = "GET")
{
    auto request = OutgoingRequest {};
    auto rest = url;
    if (rest.starts_with("https://"))
    {
        request.origin.scheme = Scheme::Https;
        rest.remove_prefix(8);
    }
    else
    {
        REQUIRE(rest.starts_with("http://"));
        rest.remove_prefix(7);
    }
    auto const slash = rest.find('/');
    auto authority = rest.substr(0, slash);
    auto const target = slash == std::string_view::npos ? std::string_view { "/" } : rest.substr(slash);
    auto const portAt = authority.rfind(':');
    if (portAt != std::string_view::npos && authority.find(']', portAt) == std::string_view::npos)
    {
        auto const digits = authority.substr(portAt + 1);
        auto port = std::uint16_t { 0 };
        auto const parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
        REQUIRE(parsed.ec == std::errc {});
        request.origin.port = port;
        authority = authority.substr(0, portAt);
    }
    request.origin.host = std::string { authority };
    auto parsedMethod = Method::parse(method);
    REQUIRE(parsedMethod.has_value());
    request.head.method = std::move(*parsedMethod);
    auto parsedTarget = RequestTarget::parse(target);
    REQUIRE(parsedTarget.has_value());
    request.head.target = std::move(*parsedTarget);
    return request;
}

/// Starts @p client sending @p request on @p loop; its answer lands in @p out.
/// @param loop The client's loop.
/// @param client The client; must outlive the flow.
/// @param request The request.
/// @param out Where the answer goes; must outlive the flow.
inline void startSend(net::EventLoop& loop,
                      HttpClient& client,
                      OutgoingRequest request,
                      std::optional<HttpResult<IncomingResponse>>* out)
{
    loop.spawn(into(client.send(std::move(request)), out));
}

/// The next chunk after @p gap on @p loop's clock, or a failure of code @p fail instead.
inline async::Task<HttpResult<BodyChunk>> chunkAfter(net::EventLoop* loop,
                                                     std::chrono::milliseconds gap,
                                                     std::string text,
                                                     BodyEnd end,
                                                     std::optional<HttpErrorCode> fail)
{
    co_await loop->delay(gap);
    if (fail.has_value())
    {
        auto error = HttpError {};
        error.code = *fail;
        co_return std::unexpected(std::move(error));
    }
    auto const bytes = bytesOf(text);
    co_return BodyChunk { .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()), .end = end };
}

/// @return No trailers.
inline async::Task<HttpResult<Fields>> noTrailers()
{
    co_return Fields {};
}

/// A body of a known length whose chunks arrive @c gap apart on the loop's clock: an upload slower
/// than a response timeout. With @c failAt set, that chunk fails (@c HttpErrorCode::InternalError)
/// instead of arriving.
class SlowBodySource final: public IBodySource
{
  public:
    /// @param loop Whose clock the gaps are measured on.
    /// @param chunks The body, chunk by chunk; must not be empty.
    /// @param gap The wait before each chunk.
    /// @param failAt The index of the chunk that fails, if one does.
    SlowBodySource(net::EventLoop& loop,
                   std::vector<std::string> chunks,
                   std::chrono::milliseconds gap,
                   std::optional<std::size_t> failAt = std::nullopt):
        _loop { &loop }, _chunks { std::move(chunks) }, _gap { gap }, _failAt { failAt }
    {
    }

    [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override
    {
        auto total = std::uint64_t { 0 };
        for (auto const& chunk: _chunks)
            total += chunk.size();
        return total;
    }

    [[nodiscard]] TrailerIntent trailerIntent() const noexcept override { return TrailerIntent::None; }

    [[nodiscard]] async::Task<HttpResult<BodyChunk>> next() override
    {
        auto const index = _next++;
        auto const end = _next == _chunks.size() ? BodyEnd::Reached : BodyEnd::More;
        auto const fail = _failAt == index ? std::optional { HttpErrorCode::InternalError } : std::nullopt;
        return chunkAfter(_loop, _gap, _chunks.at(index), end, fail);
    }

    [[nodiscard]] async::Task<HttpResult<Fields>> trailers() override { return noTrailers(); }

  private:
    net::EventLoop* _loop;
    std::vector<std::string> _chunks;
    std::chrono::milliseconds _gap;
    std::optional<std::size_t> _failAt;
    std::size_t _next = 0;
};

// -------------------------------------------------------------------------------------------------
// A served connection, driven by a case (Task 18).

/// Whether a flow a case started has finished.
enum class FlowEnd : std::uint8_t
{
    Running,  ///< It is still parked, or has not started.
    Finished, ///< It ran to its end.
};

/// Runs @p task, then marks @p end finished.
/// @param task The flow.
/// @param end Where it says so; must outlive the returned task.
inline async::Task<void> marking(async::Task<void> task, FlowEnd* end)
{
    co_await std::move(task);
    *end = FlowEnd::Finished;
}

/// Reads @p socket until EOF or an error, appending as it goes, then marks @p end finished: what a
/// client received so far, readable while the server is still writing.
/// @param socket The client end; must outlive the returned task.
/// @param out Where the bytes go; must outlive the returned task.
/// @param end Set once the wire has ended; must outlive the returned task.
inline async::Task<void> collect(net::ISocket* socket, std::string* out, FlowEnd* end)
{
    auto chunk = std::vector<std::byte>(4096);
    while (true)
    {
        auto const got = co_await socket->read(std::span<std::byte> { chunk });
        if (!got.has_value() || *got == 0)
            break;
        *out += textOf(std::span<std::byte const> { chunk }.first(*got));
    }
    *end = FlowEnd::Finished;
}

/// Writes @p text to @p socket in full, then half-closes it: a client that has sent all it will.
/// @param socket The writer; must outlive the returned task.
/// @param text The wire bytes, owned by the frame.
inline async::Task<void> writeThenFinish(net::ISocket* socket, std::string text)
{
    if (!text.empty())
        static_cast<void>(co_await socket->write(bytesOf(text)));
    static_cast<void>(co_await socket->shutdownWrite());
}

/// Whether a served connection's client end reads what the server writes.
enum class ClientReads : std::uint8_t
{
    Yes, ///< Everything, as it arrives.
    No,  ///< Nothing: a client that stops reading, which a bounded pair turns into a stalled write.
};

/// One connection served by @c serveHttpConnection over an in-memory pair on a hand-driven loop, its
/// client end a case writes raw bytes to. Everything the server wrote is in @c received as soon as a
/// drain has run; the clock moves only when the case advances it.
class ServedConnection
{
  public:
    /// Starts serving.
    /// @param handler The handler; must outlive this.
    /// @param options The server's configuration.
    /// @param maxBytesInFlight The pair's per-direction bound; zero means none.
    /// @param reads Whether the client end reads.
    ServedConnection(IHttpHandler& handler,
                     HttpServerOptions options,
                     std::size_t maxBytesInFlight = 0,
                     ClientReads reads = ClientReads::Yes):
        _pair { net::testing::InMemorySocketPair::create(maxBytesInFlight) }
    {
        _loop.spawn(marking(
            serveHttpConnection(&_loop, std::move(_pair.server), &handler, std::move(options)), &_server));
        if (reads == ClientReads::Yes)
            _loop.spawn(collect(_pair.client.get(), &_received, &_wire));
        drain(); // the server parks in its first read now, at time zero
    }

    ServedConnection(ServedConnection const&) = delete;
    ServedConnection& operator=(ServedConnection const&) = delete;
    ServedConnection(ServedConnection&&) = delete;
    ServedConnection& operator=(ServedConnection&&) = delete;
    ~ServedConnection() = default;

    /// Sends @p text and leaves the client end open, then drains.
    void send(std::string text)
    {
        _loop.spawn(writeOnly(_pair.client.get(), std::move(text)));
        drain();
    }

    /// Sends @p text and half-closes the client end, then drains.
    void finish(std::string text = {})
    {
        _loop.spawn(writeThenFinish(_pair.client.get(), std::move(text)));
        drain();
    }

    /// Runs the loop until it has nothing to do.
    void drain() { std::ignore = _loop.drain(); }

    /// Moves the clock by @p by, then drains.
    void advance(std::chrono::milliseconds by)
    {
        _clock.advance(by);
        drain();
    }

    /// @return The loop, for a handler that waits on its clock.
    [[nodiscard]] net::EventLoop& loop() noexcept { return _loop; }

    /// @return Everything the server wrote so far.
    [[nodiscard]] std::string const& received() const noexcept { return _received; }

    /// @return Whether the server's flow has ended.
    [[nodiscard]] FlowEnd server() const noexcept { return _server; }

    /// @return Whether the client end has read the wire's end (EOF or a reset).
    [[nodiscard]] FlowEnd wire() const noexcept { return _wire; }

  private:
    platform::ManualClock _clock {};
    net::testing::InMemorySocketPair _pair;
    std::string _received;
    FlowEnd _server = FlowEnd::Running;
    FlowEnd _wire = FlowEnd::Running;
    net::testing::TestLoop _loop { _clock }; // last: destroyed first, with every flow that uses the above
};

/// What a client of one served connection saw.
struct Exchanged
{
    std::string wire; ///< Every byte the server wrote.
    FlowEnd server;   ///< Whether the server's flow ended.
    FlowEnd closed;   ///< Whether the client saw the wire end.
};

/// Sends @p wire on a fresh connection and half-closes it, then reads what the server answers.
/// @param handler The handler.
/// @param options The server's configuration.
/// @param wire The client's bytes.
/// @return What came back.
[[nodiscard]] inline Exchanged exchange(IHttpHandler& handler, HttpServerOptions options, std::string wire)
{
    auto served = ServedConnection { handler, std::move(options) };
    served.finish(std::move(wire));
    return Exchanged { .wire = served.received(), .server = served.server(), .closed = served.wire() };
}

/// One response as a client read it off the wire.
struct ParsedResponse
{
    ResponseHead head; ///< Its head.
    std::string body;  ///< Its body, decoded.
    Fields trailers;   ///< Its trailers.
};

/// Every response in @p wire, in order, each framed as a response to @p method would be; a malformed
/// one fails the case, and a body cut off at the wire's end ends the list.
/// @param wire What a server wrote.
/// @param method The request method the responses answer.
/// @return The responses, 1xx ones included.
[[nodiscard]] inline std::vector<ParsedResponse> parseResponses(std::string_view wire,
                                                                std::string_view method = "GET")
{
    auto responses = std::vector<ParsedResponse> {};
    while (!wire.empty())
    {
        auto parser = HeadParser { MessageRole::Response, HttpLimits {} };
        auto fed = parser.feed(wire);
        REQUIRE(fed.has_value());
        if (fed->progress != Progress::Complete)
            break;
        wire.remove_prefix(fed->consumed);
        auto response = ParsedResponse { .head = parser.takeResponse(), .body = {}, .trailers = {} };
        auto framing =
            responseFraming(method, response.head.version, response.head.status, response.head.fields);
        REQUIRE(framing.has_value());
        switch (framing->kind)
        {
            case BodyFraming::None: break;
            case BodyFraming::Length:
                if (wire.size() < framing->length)
                    return responses;
                response.body = std::string { wire.substr(0, framing->length) };
                wire.remove_prefix(framing->length);
                break;
            case BodyFraming::UntilClose:
                response.body = std::string { wire };
                wire = {};
                break;
            case BodyFraming::Chunked: {
                auto decoder = ChunkedDecoder { MessageRole::Response, HttpLimits {} };
                while (true)
                {
                    auto step = decoder.feed(wire);
                    REQUIRE(step.has_value());
                    response.body += std::string { step->data };
                    wire.remove_prefix(step->consumed);
                    if (step->progress == Progress::Complete)
                        break;
                    if (wire.empty())
                        return responses;
                }
                response.trailers = decoder.takeTrailers();
                break;
            }
        }
        responses.push_back(std::move(response));
    }
    return responses;
}

/// @return The first value of the field @p name in @p fields, or empty.
[[nodiscard]] inline std::string fieldOf(Fields const& fields, std::string_view name)
{
    auto const values = fields.get(name);
    return values.empty() ? std::string {} : std::string { values.front() };
}

/// A body source that reads a request body as the response is written: what a handler that streams
/// its request into its response returns. It holds the reader, so the reader lives until the
/// response does.
class ReaderBodySource final: public IBodySource
{
  public:
    /// @param reader The request body.
    explicit ReaderBodySource(std::unique_ptr<IBodyReader> reader): _reader { std::move(reader) } {}

    [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override { return std::nullopt; }
    [[nodiscard]] TrailerIntent trailerIntent() const noexcept override { return TrailerIntent::Possible; }
    [[nodiscard]] async::Task<HttpResult<BodyChunk>> next() override { return _reader->read(1024); }
    [[nodiscard]] async::Task<HttpResult<Fields>> trailers() override { return _reader->trailers(); }

  private:
    std::unique_ptr<IBodyReader> _reader;
};

/// How an @c EchoHandler reads the request body.
enum class EchoMode : std::uint8_t
{
    Buffered, ///< Whole, before it answers: the response has a known length (or trailers).
    Streamed, ///< Lazily, as the server writes the response, which is chunked.
};

/// Answers every request 200 with its body and trailers, and `X-Echo: <method> <target>`. A body
/// read failure is the handler's failure, and is recorded.
class EchoHandler final: public IHttpHandler
{
  public:
    /// @param mode How the body is read.
    explicit EchoHandler(EchoMode mode = EchoMode::Buffered): _mode { mode } {}

    [[nodiscard]] async::Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) override
    {
        return answer(this, std::move(request));
    }

    /// @return How many requests it was called for.
    [[nodiscard]] std::size_t calls() const noexcept { return _calls; }

    /// @return The code of the last body read that failed, if one did.
    [[nodiscard]] std::optional<HttpErrorCode> readFailure() const noexcept { return _readFailure; }

    /// @return The peer of the last request.
    [[nodiscard]] std::optional<net::ResolvedEndpoint> const& lastPeer() const noexcept { return _peer; }

    /// @return The target of every request it was called for, in order.
    [[nodiscard]] std::vector<std::string> const& targets() const noexcept { return _targets; }

  private:
    static async::Task<HttpResult<OutgoingResponse>> answer(EchoHandler* self, IncomingRequest request)
    {
        ++self->_calls;
        self->_peer = request.peer;
        self->_targets.emplace_back(request.head.target.text());
        auto head = ResponseHead {};
        auto const echo =
            std::string { request.head.method.text() } + " " + std::string { request.head.target.text() };
        REQUIRE(head.fields.append("X-Echo", echo).has_value());
        if (self->_mode == EchoMode::Streamed)
            co_return OutgoingResponse { .head = std::move(head),
                                         .body =
                                             std::make_unique<ReaderBodySource>(std::move(request.body)) };
        auto body = co_await readAll(request.body.get(), 1024);
        if (body.error.has_value())
        {
            self->_readFailure = body.error->code;
            co_return std::unexpected(std::move(*body.error));
        }
        auto trailers = co_await request.body->trailers();
        if (!trailers.has_value())
            co_return std::unexpected(std::move(trailers.error()));
        co_return OutgoingResponse { .head = std::move(head),
                                     .body = std::make_unique<BufferBodySource>(std::move(body.text),
                                                                                std::move(*trailers)) };
    }

    EchoMode _mode;
    std::size_t _calls = 0;
    std::optional<HttpErrorCode> _readFailure;
    std::optional<net::ResolvedEndpoint> _peer;
    std::vector<std::string> _targets;
};

/// Whether an @c IgnoringHandler keeps each request's body reader alive after it answers.
enum class KeepsReader : std::uint8_t
{
    No,  ///< The reader is dropped with the request.
    Yes, ///< The handler holds it for as long as it lives.
};

/// Answers every request `200` with the body `ok`, and never reads the request body.
class IgnoringHandler final: public IHttpHandler
{
  public:
    /// @param keeps Whether the unread body readers are kept alive.
    explicit IgnoringHandler(KeepsReader keeps = KeepsReader::No): _keeps { keeps } {}

    [[nodiscard]] async::Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) override
    {
        ++_calls;
        if (_keeps == KeepsReader::Yes)
            _kept.push_back(std::move(request.body));
        return okResponse();
    }

    /// @return How many requests it was called for.
    [[nodiscard]] std::size_t calls() const noexcept { return _calls; }

    /// @return The readers it kept, in order.
    [[nodiscard]] std::vector<std::unique_ptr<IBodyReader>>& kept() noexcept { return _kept; }

    /// @return A 200 response with the body `ok`.
    static async::Task<HttpResult<OutgoingResponse>> okResponse()
    {
        co_return OutgoingResponse { .head = ResponseHead {},
                                     .body = std::make_unique<BufferBodySource>("ok") };
    }

  private:
    KeepsReader _keeps;
    std::size_t _calls = 0;
    std::vector<std::unique_ptr<IBodyReader>> _kept;
};

/// What a body read a handler left running found: the body it read, the failure that ended it, and
/// what a read retried after that failure answered.
struct LeftRunning
{
    std::string text;                     ///< The body bytes it read.
    std::optional<HttpErrorCode> failure; ///< What ended the read, if anything did.
    std::optional<HttpErrorCode> retry;   ///< What one more read after the failure answered.
    FlowEnd end = FlowEnd::Running;       ///< Whether the read's flow has ended.
};

/// Reads @p reader to its end and, after a failure, once more: a handler's read that runs on after it.
/// @param reader The body, owned by the flow.
/// @param out What it found; must outlive the returned task.
inline async::Task<void> readThenRetry(std::unique_ptr<IBodyReader> reader, LeftRunning* out)
{
    auto body = co_await readAll(reader.get(), 1024);
    out->text = body.text;
    if (body.error.has_value())
    {
        out->failure = body.error->code;
        auto again = co_await reader->read(1024);
        if (!again.has_value())
            out->retry = again.error().code;
    }
    out->end = FlowEnd::Finished;
}

/// Answers every request `200` with the body `ok` at once, and leaves a flow reading its body running
/// on the loop: a handler whose body read outlives its response.
class SpawningReader final: public IHttpHandler
{
  public:
    /// @param loop Where the read is spawned, set before the first request; must outlive this.
    /// @param out What the last read found; must outlive every read.
    /// @param answerAfter How long it waits on the loop's clock before it answers; zero answers at once.
    SpawningReader(net::EventLoop** loop,
                   LeftRunning* out,
                   std::chrono::milliseconds answerAfter = std::chrono::milliseconds { 0 }):
        _loop { loop }, _out { out }, _answerAfter { answerAfter }
    {
    }

    [[nodiscard]] async::Task<HttpResult<OutgoingResponse>> handle(IncomingRequest request) override
    {
        (*_loop)->spawn(readThenRetry(std::move(request.body), _out));
        return answer(*_loop, _answerAfter);
    }

  private:
    static async::Task<HttpResult<OutgoingResponse>> answer(net::EventLoop* loop,
                                                            std::chrono::milliseconds after)
    {
        if (after > std::chrono::milliseconds { 0 })
            co_await loop->delay(after);
        co_return co_await IgnoringHandler::okResponse();
    }

    net::EventLoop** _loop;
    LeftRunning* _out;
    std::chrono::milliseconds _answerAfter;
};

// -------------------------------------------------------------------------------------------------
// TLS, scripted (R29).

/// How a @c ScriptedTlsSocket's handshake ends.
enum class HandshakeOutcome : std::uint8_t
{
    Succeeds,          ///< At once.
    RefusedByTls,      ///< With @c TlsScript::failure reported by the session, and an error.
    CutByTransport,    ///< With @c TlsScript::transportError and no TLS failure, as a reset would.
    ParksThenSucceeds, ///< After @c TlsScript::park on the loop's clock, honouring a stop.
};

/// What a @c ScriptedTlsSocket's reads do once the handshake has succeeded.
enum class ReadOutcome : std::uint8_t
{
    Forwards,     ///< They read the transport.
    RefusedByTls, ///< The first fails, the session reporting @c TlsScript::failure, as an alert would.
};

/// What a @c ScriptedTlsContext's connections do.
struct TlsScript
{
    std::optional<net::NetError> wrapError; ///< What @c wrapClient answers instead of a socket, if set.
    HandshakeOutcome handshake = HandshakeOutcome::Succeeds; ///< How the handshake ends.
    ReadOutcome reads = ReadOutcome::Forwards;               ///< What the reads after it do.
    net::TlsFailure failure;                                 ///< What a refusal reports.
    net::NetError transportError =
        net::makeNetError(net::NetErrorCode::ConnReset); ///< A cut handshake's error.
    std::chrono::milliseconds park { 0 };                ///< How long a parked handshake waits.
};

/// @return The script whose handshake ends as @p handshake, everything else as the defaults say.
[[nodiscard]] inline TlsScript scriptOf(HandshakeOutcome handshake)
{
    auto script = TlsScript {};
    script.handshake = handshake;
    return script;
}

/// What a scripted context's connections saw, kept by the context so it outlives them.
struct TlsRecord
{
    std::vector<std::string> serverNames;  ///< The @c net::TlsPeer::serverName of every wrapClient.
    std::size_t wraps = 0;                 ///< How many sockets were wrapped.
    std::size_t handshakes = 0;            ///< How many handshakes were asked for.
    std::size_t writes = 0;                ///< How many writes reached the TLS sockets.
    std::size_t writesBeforeHandshake = 0; ///< How many of them came before a handshake had succeeded.
};

/// The session of a @c ScriptedTlsSocket: it reports a failure once one has been scripted to happen.
class ScriptedTlsSession final: public net::ITlsSession
{
  public:
    ScriptedTlsSession() = default;

    [[nodiscard]] std::expected<std::array<std::byte, 32>, std::string> channelBinding() const override
    {
        return std::unexpected(std::string { "scripted" });
    }
    [[nodiscard]] std::optional<std::array<std::byte, 32>> peerCertificateDigest() const override
    {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<net::TlsVersion> negotiatedVersion() const override { return std::nullopt; }
    [[nodiscard]] std::optional<net::TlsFailure> failure() const override { return _failure; }

    /// Makes @c failure report @p failure from now on.
    void fail(net::TlsFailure failure) { _failure = std::move(failure); }

  private:
    std::optional<net::TlsFailure> _failure;
};

/// A "TLS" socket that encrypts nothing: it forwards to the transport it owns, and its handshake and
/// reads end as its @c TlsScript says.
class ScriptedTlsSocket final: public net::ISocket
{
  public:
    ScriptedTlsSocket(std::unique_ptr<net::ISocket> inner,
                      net::EventLoop& loop,
                      TlsScript script,
                      TlsRecord& record):
        _inner { std::move(inner) }, _loop { &loop }, _script { std::move(script) }, _record { &record }
    {
    }

    [[nodiscard]] net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        if (_script.reads == ReadOutcome::RefusedByTls)
        {
            _session.fail(_script.failure);
            return net::IoAwaitable { std::unexpected(
                net::makeNetError(net::NetErrorCode::SystemError, 0, "scripted TLS record refused")) };
        }
        return _inner->read(buffer);
    }

    [[nodiscard]] net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        countWrite();
        return _inner->write(buffer);
    }

    [[nodiscard]] net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                 std::shared_ptr<void const> keepAlive = {}) override
    {
        countWrite();
        return _inner->writeVectored(segments, std::move(keepAlive));
    }

    [[nodiscard]] net::ResultAwaitable<void> handshakeIfNeeded() override
    {
        ++_record->handshakes;
        switch (_script.handshake)
        {
            case HandshakeOutcome::Succeeds:
                _isEstablished = Established::Yes;
                return net::ResultAwaitable<void> { net::ResultAwaitable<void>::Result {} };
            case HandshakeOutcome::RefusedByTls:
                _session.fail(_script.failure);
                return net::ResultAwaitable<void> { net::ResultAwaitable<void>::Result { std::unexpected(
                    net::makeNetError(net::NetErrorCode::SystemError, 0, "scripted handshake refused")) } };
            case HandshakeOutcome::CutByTransport:
                return net::ResultAwaitable<void> { net::ResultAwaitable<void>::Result {
                    std::unexpected(_script.transportError) } };
            case HandshakeOutcome::ParksThenSucceeds:
                return net::ResultAwaitable<void> { parkThenSucceed(this) };
        }
        return net::ResultAwaitable<void> { net::ResultAwaitable<void>::Result {} };
    }

    void cancelRead() noexcept override { _inner->cancelRead(); }
    [[nodiscard]] net::ITlsSession const* tlsSession() const noexcept override { return &_session; }
    void close() noexcept override { _inner->close(); }
    [[nodiscard]] bool isClosed() const noexcept override { return _inner->isClosed(); }

  private:
    /// Whether a handshake has succeeded yet.
    enum class Established : std::uint8_t
    {
        No,
        Yes,
    };

    static async::Task<std::expected<void, net::NetError>> parkThenSucceed(ScriptedTlsSocket* self)
    {
        co_await self->_loop->delay(self->_script.park);
        self->_isEstablished = Established::Yes;
        co_return std::expected<void, net::NetError> {};
    }

    void countWrite() noexcept
    {
        ++_record->writes;
        if (_isEstablished == Established::No)
            ++_record->writesBeforeHandshake;
    }

    std::unique_ptr<net::ISocket> _inner;
    net::EventLoop* _loop;
    TlsScript _script;
    TlsRecord* _record;
    ScriptedTlsSession _session;
    Established _isEstablished = Established::No;
};

/// A client context that binds the name it is given, and whose connections do what @c TlsScript says.
class ScriptedTlsContext final: public net::ITlsContext
{
  public:
    /// @param loop Where a parked handshake waits.
    /// @param script What every connection does.
    ScriptedTlsContext(net::EventLoop& loop, TlsScript script): _loop { &loop }, _script { std::move(script) }
    {
    }

    [[nodiscard]] std::unique_ptr<net::ISocket> wrap(std::unique_ptr<net::ISocket> inner,
                                                     async::IExecutor& /*executor*/) override
    {
        ++_record.wraps;
        return std::make_unique<ScriptedTlsSocket>(std::move(inner), *_loop, _script, _record);
    }

    [[nodiscard]] std::expected<std::unique_ptr<net::ISocket>, net::NetError> wrapClient(
        std::unique_ptr<net::ISocket> inner, async::IExecutor& executor, net::TlsPeer const& peer) override
    {
        _record.serverNames.push_back(peer.serverName);
        if (_script.wrapError.has_value())
            return std::unexpected(*_script.wrapError);
        return wrap(std::move(inner), executor);
    }

    /// @return What its connections saw.
    [[nodiscard]] TlsRecord const& record() const noexcept { return _record; }

  private:
    net::EventLoop* _loop;
    TlsScript _script;
    TlsRecord _record;
};

/// A client context written before @c net::ITlsContext::wrapClient existed: it overrides @c wrap only,
/// so the default @c wrapClient refuses any server name (R23), as every legacy context's does.
class LegacyTlsContext final: public net::ITlsContext
{
  public:
    LegacyTlsContext() = default;

    [[nodiscard]] std::unique_ptr<net::ISocket> wrap(std::unique_ptr<net::ISocket> inner,
                                                     async::IExecutor& /*executor*/) override
    {
        ++_wraps;
        return inner;
    }

    /// @return How many sockets were wrapped.
    [[nodiscard]] std::size_t wraps() const noexcept { return _wraps; }

  private:
    std::size_t _wraps = 0;
};

} // namespace core::http::test
