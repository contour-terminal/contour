// SPDX-License-Identifier: Apache-2.0
#include <net/HttpServer.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <net/AsyncBufferedReader.hpp>

namespace net
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

    /// Trims leading and trailing ASCII whitespace from a view.
    [[nodiscard]] std::string_view trim(std::string_view s) noexcept
    {
        constexpr auto Whitespace = std::string_view { " \t\r\n\f\v" };
        auto const first = s.find_first_not_of(Whitespace);
        if (first == std::string_view::npos)
            return {};
        auto const last = s.find_last_not_of(Whitespace);
        return s.substr(first, last - first + 1);
    }

    /// Parses the request line and headers from @p headerText into @p request.
    /// @param headerText The head block, delimiter excluded.
    /// @param request The request to populate (not owned).
    /// @return The advertised Content-Length, or std::nullopt if the request line
    ///         was malformed (fewer than three space-separated tokens).
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

            if (line.empty())
                continue;

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
            auto const name = trim(line.substr(0, colon));
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
    coro::Task<void> handleConnection(ISocket* socket, HttpHandler const* handler, HttpLimits limits)
    {
        auto request = co_await readRequest(socket, limits);
        if (!request.has_value())
        {
            // Answer what we can diagnose; a peer that vanished gets nothing.
            if (request.error().code != NetErrorCode::Eof)
            {
                auto const status = request.error().code == NetErrorCode::MessageTooLarge ? 413 : 400;
                static_cast<void>(
                    co_await writeResponse(socket, HttpResponse::withStatus(status, std::string {})));
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
        catch (coro::OperationCancelled const&)
        {
            throw;
        }
        catch (...)
        {
            response = HttpResponse::withStatus(500, std::string {});
        }
        static_cast<void>(co_await writeResponse(socket, std::move(response)));
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

coro::Task<std::expected<HttpRequest, NetError>> readRequest(ISocket* socket, HttpLimits limits)
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
        co_return std::unexpected(makeNetError(NetErrorCode::Other, 0, "malformed request head"));

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

coro::Task<IoResult> writeResponse(ISocket* socket, HttpResponse response)
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

coro::Task<void> serve(IListener* listener, HttpHandler handler, HttpLimits limits)
{
    while (true)
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value())
            co_return; // listener closed or cancelled
        auto conn = std::move(*accepted);
        co_await handleConnection(conn.get(), &handler, limits);
    }
}

} // namespace net
