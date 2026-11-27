// SPDX-License-Identifier: Apache-2.0
#include <core/http/testing/ScriptedConnector.hpp>

#include <core/net/testing/InMemorySocket.hpp>

#include <utility>

namespace core::http::testing
{

ScriptedConnector::ScriptedConnector(std::size_t maxBytesInFlight): _maxBytesInFlight { maxBytesInFlight }
{
}

ScriptedConnector::~ScriptedConnector() = default;

async::Task<net::SocketResult> ScriptedConnector::connect(std::string host,
                                                          std::uint16_t port,
                                                          net::DialOptions options)
{
    _dials.push_back(ScriptedDial { .host = std::move(host), .port = port, .options = options });
    if (!_refusals.empty())
    {
        auto error = std::move(_refusals.front());
        _refusals.pop_front();
        co_return std::unexpected(std::move(error));
    }
    auto pair = net::testing::InMemorySocketPair::create(_maxBytesInFlight);
    _accepted.push_back(std::move(pair.server));
    co_return std::unique_ptr<net::ISocket> { std::move(pair.client) };
}

void ScriptedConnector::refuseNext(net::NetError error)
{
    _refusals.push_back(std::move(error));
}

std::unique_ptr<net::ISocket> ScriptedConnector::acceptNext()
{
    if (_accepted.empty())
        return nullptr;
    auto socket = std::move(_accepted.front());
    _accepted.pop_front();
    return socket;
}

std::size_t ScriptedConnector::connects() const noexcept
{
    return _dials.size();
}

std::span<ScriptedDial const> ScriptedConnector::dials() const noexcept
{
    return _dials;
}

} // namespace core::http::testing
