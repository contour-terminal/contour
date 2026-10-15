// SPDX-License-Identifier: Apache-2.0
#include <core/net/HttpServer.hpp>

#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/AsyncBufferedReader.hpp>
#include <core/net/Diagnostics.hpp>
#include <core/net/InterruptibleSleep.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace core::net
{

namespace
{
    /// The status codes this server names, and their reason phrases. Adding a code
    /// is a row here, not a branch in the serializer.
    struct StatusReason
    {
        int status;              ///< The HTTP status code.
        std::string_view reason; ///< Its reason phrase.
    };

    constexpr auto StatusReasons = std::array {
        StatusReason { 200, "OK" },
        StatusReason { 201, "Created" },
        StatusReason { 202, "Accepted" },
        StatusReason { 204, "No Content" },
        StatusReason { 301, "Moved Permanently" },
        StatusReason { 302, "Found" },
        StatusReason { 304, "Not Modified" },
        StatusReason { 400, "Bad Request" },
        StatusReason { 401, "Unauthorized" },
        StatusReason { 403, "Forbidden" },
        StatusReason { 404, "Not Found" },
        StatusReason { 405, "Method Not Allowed" },
        StatusReason { 408, "Request Timeout" },
        StatusReason { 413, "Content Too Large" },
        StatusReason { 415, "Unsupported Media Type" },
        StatusReason { 429, "Too Many Requests" },
        StatusReason { 500, "Internal Server Error" },
        StatusReason { 501, "Not Implemented" },
        StatusReason { 503, "Service Unavailable" },
    };

    /// Case-insensitively compares two ASCII strings for equality.
    [[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(
            a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    }

    /// The ASCII whitespace a field VALUE may be padded with — and which a field NAME, being a
    /// token, may not contain anywhere.
    constexpr auto Whitespace = std::string_view { " \t\r\n\f\v" };

    /// Trims leading and trailing ASCII whitespace from a view.
    [[nodiscard]] std::string_view trim(std::string_view s) noexcept
    {
        auto const first = s.find_first_not_of(Whitespace);
        if (first == std::string_view::npos)
            return {};
        auto const last = s.find_last_not_of(Whitespace);
        return s.substr(first, last - first + 1);
    }

    /// Parses the request line and headers from @p headerText into @p request.
    /// @param headerText The head block, delimiter excluded.
    /// @param request The request to populate (not owned).
    /// @return The advertised Content-Length, or std::nullopt when the head must be
    ///         refused: a malformed request line (fewer than three space-separated
    ///         tokens), an obs-fold continuation, a header without a colon, a field
    ///         name that is empty or carries whitespace before its colon, an
    ///         unparsable or conflicting Content-Length, a Transfer-Encoding, or a
    ///         blank line with bytes still behind it (see the empty-line case below).
    [[nodiscard]] std::optional<std::size_t> parseHead(std::string_view headerText, HttpRequest* request)
    {
        auto contentLength = std::size_t { 0 };
        auto sawContentLength = false;
        auto lineStart = std::size_t { 0 };
        auto firstLine = true;

        while (lineStart <= headerText.size())
        {
            // Split on LF and strip an optional CR, so a client using bare-LF line
            // endings parses as the same set of lines rather than as one giant first
            // line whose interior spaces would then populate method/path/version
            // with garbage. readLine in this module is equally tolerant.
            auto lineEnd = headerText.find('\n', lineStart);
            auto const nextStart = (lineEnd == std::string_view::npos) ? headerText.size() + 1 : lineEnd + 1;
            if (lineEnd == std::string_view::npos)
                lineEnd = headerText.size();
            auto lineStop = lineEnd;
            if (lineStop > lineStart && headerText[lineStop - 1] == '\r')
                --lineStop;
            auto const line = headerText.substr(lineStart, lineStop - lineStart);
            lineStart = nextStart;

            if (firstLine)
            {
                firstLine = false;
                auto const sp1 = line.find(' ');
                auto const sp2 = (sp1 == std::string_view::npos) ? sp1 : line.find(' ', sp1 + 1);
                if (sp1 == std::string_view::npos || sp2 == std::string_view::npos)
                    return std::nullopt; // not a request line
                request->method = std::string { line.substr(0, sp1) };
                request->path = std::string { line.substr(sp1 + 1, sp2 - sp1 - 1) };
                request->version = std::string { line.substr(sp2 + 1) };
                continue;
            }

            // The head ends at the FIRST empty line, whichever terminator produced it.
            // Skipping it and carrying on was a request-smuggling desync: a front-end that
            // honours a bare LF as a line terminator (RFC 9112 §2.2 permits it, and this
            // parser does it for every other line) reads "GET / HTTP/1.1\n\nHost: evil…" as
            // TWO requests, while folding those headers into the first makes it one — the
            // two ends then disagree about where the next request begins.
            //
            // The bytes after that empty line were already consumed as part of the head
            // block readUntil("\r\n\r\n") delivered, so they can be neither re-framed as
            // the body nor pushed back for the next request: any Content-Length parsed
            // before the empty line would index into the wrong place. An ambiguous
            // message is therefore refused, as Transfer-Encoding and a conflicting
            // Content-Length already are, rather than resolved by guessing.
            if (line.empty())
            {
                if (lineStart < headerText.size())
                    return std::nullopt; // bytes follow the blank line: ambiguous framing
                break;                   // the blank line is the block's end: the head is complete
            }

            // An obs-fold continuation (a header line starting with SP/HTAB) belongs
            // to the previous header's value. RFC 9112 §5.2 deprecates it and permits
            // rejecting the message; dropping it silently is the one thing we must
            // not do, since a folded Content-Length would otherwise vanish and leave
            // the body unread on a connection we believe fully parsed.
            if (line.front() == ' ' || line.front() == '\t')
                return std::nullopt;

            auto const colon = line.find(':');
            if (colon == std::string_view::npos)
                return std::nullopt; // a header line without a colon is not a header

            // RFC 9112 §5.1 makes this a MUST: a server rejects any request that carries
            // whitespace between a field name and its colon. Trimming it instead is the same
            // class of defect as the bare-LF blank line above — a front-end that trims and a
            // server that rejects (or the reverse) disagree about where the field name ends, and
            // so about what the message says. A field name is a token, so no whitespace belongs
            // anywhere inside it, and an empty one is not a name at all. (A name whose whitespace
            // LEADS the line is already refused as an obs-fold continuation above.)
            auto const name = line.substr(0, colon);
            if (name.empty() || name.find_first_of(Whitespace) != std::string_view::npos)
                return std::nullopt;

            // The VALUE keeps its trim: RFC 9112 §5 pads field-value with optional whitespace on
            // both sides that a recipient removes, which is the ordinary "Host: example" spelling.
            auto const value = trim(line.substr(colon + 1));
            request->headers.emplace_back(std::string { name }, std::string { value });

            if (iequals(name, "Content-Length"))
            {
                auto parsed = std::size_t { 0 };
                auto const* const begin = value.data();
                auto const [ptr, ec] = std::from_chars(begin, begin + value.size(), parsed);
                if (ec != std::errc {} || ptr != begin + value.size())
                    return std::nullopt; // unparsable length: refuse rather than guess
                // A repeated Content-Length is a request-smuggling vector when it
                // disagrees with the first; RFC 9112 §6.3 requires rejecting it.
                if (sawContentLength && parsed != contentLength)
                    return std::nullopt;
                contentLength = parsed;
                sawContentLength = true;
            }

            // Chunked bodies are out of scope for this server, so a request that
            // announces one must be refused rather than parsed as a zero-length body
            // that leaves its payload buffered as if it were the next request.
            if (iequals(name, "Transfer-Encoding"))
                return std::nullopt;
        }
        return contentLength;
    }

    /// Handles one accepted connection: read a request, dispatch, write the response.
    async::Task<void> handleConnection(ISocket* socket, HttpHandler const* handler, HttpLimits limits)
    {
        auto request = co_await readRequest(socket, limits);
        if (!request.has_value())
        {
            // Answer what we can diagnose; a peer that vanished gets nothing.
            if (request.error().code != NetErrorCode::Eof)
            {
                auto const status = request.error().code == NetErrorCode::MessageTooLarge ? 413 : 400;
                std::ignore =
                    co_await writeResponse(socket, HttpResponse::withStatus(status, std::string {}));
                // The request was refused before it was read to its end, so bytes of it may still
                // be unread here, and a bare close over them is a reset that destroys the refusal
                // (core-cpp#35). No loop is at hand in `serve`, so each read of the drain carries
                // its share of the bound.
                std::ignore = co_await closeLingering(socket, nullptr, limits.linger);
            }
            co_return;
        }

        // The handler is caller-supplied code. An exception escaping it would unwind
        // through the accept loop and take the whole server down — one bad request
        // ending every future connection — so it is contained here and answered as a
        // 500. Cancellation is NOT caught: it is how a shutdown unwinds this flow.
        auto response = HttpResponse {};
        try
        {
            response = (*handler)(*request);
        }
        catch (async::OperationCancelled const&)
        {
            throw;
        }
        catch (...)
        {
            response = HttpResponse::withStatus(500, std::string {});
        }
        std::ignore = co_await writeResponse(socket, std::move(response));
    }
} // namespace

std::string HttpRequest::header(std::string_view name) const
{
    for (auto const& [key, value]: headers)
        if (iequals(key, name))
            return value;
    return {};
}

std::string_view reasonPhrase(int status) noexcept
{
    auto const it = std::ranges::find(StatusReasons, status, &StatusReason::status);
    return it != StatusReasons.end() ? it->reason : std::string_view { "Unknown" };
}

HttpResponse HttpResponse::ok(std::string text)
{
    return withStatus(200, std::move(text));
}

HttpResponse HttpResponse::withStatus(int status, std::string text)
{
    return HttpResponse { .status = status,
                          .reason = std::string { reasonPhrase(status) },
                          .headers = {},
                          .body = std::move(text) };
}

async::Task<std::expected<HttpRequest, NetError>> readRequest(ISocket* socket, HttpLimits limits)
{
    // The head bound doubles as the reader's message bound, so an unterminated
    // header block is refused as it is buffered rather than after the fact. The
    // reader checks between refills, so the refusal fires within one read chunk of
    // the bound — a memory cap, not an exact byte count.
    auto reader = AsyncBufferedReader { socket, limits.maxHeadBytes };

    auto head = co_await reader.readUntil("\r\n\r\n");
    if (!head.has_value())
        co_return std::unexpected(head.error());

    auto request = HttpRequest {};
    auto const contentLength = parseHead(*head, &request);
    if (!contentLength.has_value())
        co_return std::unexpected(makeNetError(NetErrorCode::SystemError, 0, "malformed request head"));

    if (*contentLength > limits.maxBodyBytes)
        co_return std::unexpected(
            makeNetError(NetErrorCode::MessageTooLarge, 0, "request body exceeds bound"));

    if (*contentLength > 0)
    {
        auto body = co_await reader.readExactly(*contentLength);
        if (!body.has_value())
            co_return std::unexpected(body.error());
        request.body = std::move(*body);
    }

    co_return request;
}

async::Task<IoResult> writeResponse(ISocket* socket, HttpResponse response)
{
    auto out = std::string {};
    out += "HTTP/1.1 ";
    out += std::to_string(response.status);
    out += ' ';
    out += response.reason.empty() ? std::string { reasonPhrase(response.status) } : response.reason;
    out += "\r\n";

    // Framing headers are ours to decide: the body we are about to write determines
    // the length, and this server always closes. A handler that set either would
    // otherwise produce a response with two conflicting Content-Length headers,
    // which clients reject and proxies read as a smuggling signal.
    auto hasContentType = false;
    for (auto const& [name, value]: response.headers)
    {
        if (iequals(name, "Content-Length") || iequals(name, "Connection"))
            continue;
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
        if (iequals(name, "Content-Type"))
            hasContentType = true;
    }
    if (!hasContentType)
        out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "Content-Length: ";
    out += std::to_string(response.body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += response.body;

    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(out.data()), out.size() };
    co_return co_await socket->write(bytes);
}

namespace
{

    /// Hands one report of the accept loop to where @p reporting says, or to @c reportDiagnostic when
    /// it names nowhere: a loop that degrades or gives up is never silent by default.
    /// @param reporting Where reports go.
    /// @param event The report.
    void report(AcceptLoopReporting const& reporting, AcceptLoopEvent const& event)
    {
        if (reporting.onEvent)
            reporting.onEvent(event);
        else
            reportDiagnostic(event.line);
    }

    /// Waits out an accept backoff, unless the listener is closed or the flow is cancelled first.
    /// @param loop The loop whose timer waits.
    /// @param closed The listener's close token.
    /// @param delay How long.
    /// @return Whether the loop may accept again: false when the listener was closed (or destroyed)
    ///         or the flow was cancelled while it waited.
    async::Task<bool> backOff(EventLoop* loop, async::StopToken closed, std::chrono::milliseconds delay)
    {
        try
        {
            auto const woke = co_await interruptibleSleepUntil(loop, closed, loop->clock().now() + delay);
            co_return woke == WakeReason::Deadline;
        }
        catch (async::OperationCancelled const&)
        {
            // The flow's own stop, met in a backoff: `serve` returns, as it does when the same stop
            // meets it parked in an accept, which the listener answers `Cancelled`.
            co_return false;
        }
    }

} // namespace

async::Task<void> serve(EventLoop* loop,
                        IListener* listener,
                        HttpHandler handler,
                        HttpLimits limits,
                        AcceptLoopReporting reporting)
{
    // Taken while the listener is alive -- the caller's contract -- and kept: the token outlives the
    // listener, so asking it before every accept is how this loop never calls into a listener its
    // owner closed and destroyed while the loop was backing off or serving a connection.
    auto const closed = listener->closeToken();
    auto policy = AcceptErrorPolicy {};
    // Set when the listener was closed or the flow cancelled where the loop could see it; the end
    // that follows is the owner's, and only the way out below says anything about it.
    auto stopping = false;
    while (!stopping && !closed.stop_requested())
    {
        auto accepted = AcceptResult {};
        try
        {
            accepted = co_await listener->accept();
        }
        catch (async::OperationCancelled const&)
        {
            // The flow's own stop, met in an accept that answers it by THROWING `OperationCancelled`
            // -- one parked on a loop timer, say, or any `ResultAwaitable` resumed without a value --
            // where core-cpp's socket listeners answer `Cancelled`. Either way `serve` returns, and
            // the way out still says whether a degraded loop stopped.
            stopping = true;
            continue;
        }
        if (!accepted.has_value())
        {
            // One failed accept is almost never a failed listener: only a closed or dead one ends
            // the loop, and everything else is accepted past (`AcceptPolicy.hpp`).
            auto const& error = accepted.error();
            auto const verdict = policy.onError(error.code, loop->clock().now());
            switch (verdict.action)
            {
                case AcceptAction::Stop: stopping = true; continue;
                case AcceptAction::GiveUp:
                    // A dead listener left open would go on queueing handshakes nobody accepts: the
                    // port is closed, so it refuses them, and then the end is reported.
                    if (!closed.stop_requested())
                        listener->close();
                    report(reporting,
                           AcceptLoopEvent { .surface = reporting.surface,
                                             .line = describeAcceptLoopEnded(reporting.surface, error),
                                             .error = error,
                                             .kind = AcceptLoopEventKind::GaveUp });
                    co_return;
                case AcceptAction::AcceptAgain: break;
            }
            if (verdict.change == AcceptConditionChange::Degraded)
                report(reporting,
                       AcceptLoopEvent { .surface = reporting.surface,
                                         .line =
                                             describeAcceptDegraded(reporting.surface, error, verdict.streak),
                                         .error = error,
                                         .kind = AcceptLoopEventKind::Degraded });
            else if (verdict.change == AcceptConditionChange::Recovered)
                report(reporting,
                       AcceptLoopEvent { .surface = reporting.surface,
                                         .line = describeAcceptRecovered(reporting.surface, verdict.streak),
                                         .error = {},
                                         .kind = AcceptLoopEventKind::Recovered });
            if (verdict.warning.has_value())
                report(reporting,
                       AcceptLoopEvent { .surface = reporting.surface,
                                         .line = describeAcceptFailure(reporting.surface, error, verdict),
                                         .error = error,
                                         .kind = AcceptLoopEventKind::Warning });
            if (verdict.delay > std::chrono::milliseconds {})
                stopping = !co_await backOff(loop, closed, verdict.delay);
            continue;
        }
        if (auto const recovered = policy.onAccepted(loop->clock().now());
            recovered.change == AcceptConditionChange::Recovered)
            report(reporting,
                   AcceptLoopEvent { .surface = reporting.surface,
                                     .line = describeAcceptRecovered(reporting.surface, recovered.streak),
                                     .error = {},
                                     .kind = AcceptLoopEventKind::Recovered });
        auto conn = std::move(*accepted);
        // Once per connection, before anything frames the stream: a transport that negotiates
        // (TLS) has finished doing so before the first request byte is read. A connection whose
        // handshake fails has no channel to answer on, so it is dropped unanswered -- and closed
        // by `conn`'s destructor, as every connection not refused mid-request is.
        if (auto const handshaken = co_await conn->handshakeIfNeeded(); !handshaken.has_value())
            continue;
        co_await handleConnection(conn.get(), &handler, limits);
    }
    // Every way out but giving up comes here: a closed listener, a destroyed one, a cancelled flow.
    // A loop that was degraded says it has stopped, or a liveness registry goes on showing a surface
    // that was shut down as degraded -- and, restarted under the same name, the new loop has a
    // fresh policy that never reports the recovery that would clear it.
    if (policy.degraded())
        report(reporting,
               AcceptLoopEvent { .surface = reporting.surface,
                                 .line = describeAcceptLoopStopped(reporting.surface),
                                 .error = {},
                                 .kind = AcceptLoopEventKind::Stopped });
}

} // namespace core::net
