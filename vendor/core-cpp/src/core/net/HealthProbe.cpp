// SPDX-License-Identifier: Apache-2.0
#include <core/net/HealthProbe.hpp>

#include <core/async/SyncRun.hpp>
#include <core/net/BlockingConnector.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace core::net
{

namespace
{
    /// Enough for any status line; the probe reads no further than the first line break.
    constexpr std::size_t StatusLineBound = 256;

    /// @param response What the peer sent, at least up to its first line break.
    /// @return The status code of an `HTTP/1.x <code> ...` line, or why the line is not one.
    [[nodiscard]] std::expected<unsigned, NetError> parseStatusLine(std::string_view response)
    {
        auto const notHttp = [] {
            return std::unexpected(makeNetError(NetErrorCode::Unsupported, 0, "not an HTTP/1.x status line"));
        };
        if (!response.starts_with("HTTP/1."))
            return notHttp();
        auto const space = response.find(' ');
        if (space == std::string_view::npos || space + 4 > response.size())
            return notHttp();

        auto const digits = response.substr(space + 1, 3);
        // What follows the three digits must END them: `2000` is not `200`.
        auto const after = space + 4 < response.size() ? response[space + 4] : '\0';
        if (after != ' ' && after != '\r' && after != '\n')
            return notHttp();

        auto code = 0U;
        for (auto const digit: digits)
        {
            if (digit < '0' || digit > '9')
                return notHttp();
            code = (code * 10) + static_cast<unsigned>(digit - '0');
        }
        return code;
    }

    /// Sends @p request whole: `ISocket::write` resolves only once every byte is gone.
    /// @return The count sent, or why not.
    async::Task<IoResult> sendRequest(ISocket* socket, std::string request)
    {
        co_return co_await socket->write(std::as_bytes(std::span { request }));
    }

    /// Reads until the first line break, the bound, or the end of the stream.
    /// @return What arrived, or the error that stopped the read before anything did.
    async::Task<std::expected<std::string, NetError>> readStatusLine(ISocket* socket)
    {
        auto response = std::string {};
        auto chunk = std::array<char, StatusLineBound> {};
        while (response.size() < StatusLineBound && !response.contains('\n'))
        {
            auto const room = std::min(chunk.size(), StatusLineBound - response.size());
            auto const got = co_await socket->read(std::as_writable_bytes(std::span { chunk }.first(room)));
            if (!got.has_value())
            {
                if (response.empty())
                    co_return std::unexpected(got.error());
                break;
            }
            if (*got == 0)
                break;
            response.append(chunk.data(), *got);
        }
        co_return response;
    }
} // namespace

std::expected<unsigned, NetError> probeHttpStatus(std::string_view host,
                                                  std::uint16_t port,
                                                  std::string_view path,
                                                  std::chrono::milliseconds timeout)
{
    // Every phase bounded: the dial by its budget, and every later send and receive by the socket's
    // own deadlines -- a peer that accepts the connection and never answers is the ordinary case for
    // a wedged daemon, which is exactly when a health check runs.
    auto connector =
        BlockingConnector { defaultAddressResolver(), BlockingConnectorOptions { .ioTimeout = timeout } };
    auto connected = async::syncRun(connector.connect(
        std::string { host }, port, DialOptions { .connectTimeout = timeout, .keepAlive = KeepAlive::No }));
    if (!connected.has_value())
        return std::unexpected(std::move(connected.error()));
    auto const socket = std::move(*connected);

    auto request = std::string { "GET " };
    request.append(path).append(" HTTP/1.0\r\nHost: ").append(host).append("\r\nConnection: close\r\n\r\n");
    if (auto const sent = async::syncRun(sendRequest(socket.get(), request)); !sent.has_value())
        return std::unexpected(sent.error());

    auto response = async::syncRun(readStatusLine(socket.get()));
    if (!response.has_value())
        return std::unexpected(std::move(response.error()));
    return parseStatusLine(*response);
}

bool httpHealthProbe(std::string_view host,
                     std::uint16_t port,
                     std::string_view path,
                     std::chrono::milliseconds timeout)
{
    auto const status = probeHttpStatus(host, port, path, timeout);
    return status.has_value() && *status == 200;
}

} // namespace core::net
