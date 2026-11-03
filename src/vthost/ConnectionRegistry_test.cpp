// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/ScriptedBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>

#include <vthost/ConnectionRegistry.hpp>

namespace
{

/// A connection that reads until its socket fails or ends, counting how many ended.
struct ReadingConnections
{
    int ended = 0;

    [[nodiscard]] vthost::ConnectionHandler handler()
    {
        return [this](vthost::ConnectionId const& /*id*/, std::unique_ptr<core::net::ISocket> socket) {
            return serve(std::move(socket));
        };
    }

    [[nodiscard]] core::async::Task<void> serve(std::unique_ptr<core::net::ISocket> socket)
    {
        auto buffer = std::array<std::byte, 16> {};
        auto const read = co_await socket->read(buffer);
        std::ignore = read;
        ++ended;
    }
};

} // namespace

TEST_CASE("closeAll ends every tracked connection and the registry forgets them", "[vthost][connections]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto connections = ReadingConnections {};
    auto const registry = vthost::ConnectionRegistry {};
    auto const handler = registry.tracked(connections.handler());

    auto first = core::net::testing::InMemorySocketPair::create();
    auto second = core::net::testing::InMemorySocketPair::create();
    loop.spawn(
        handler(vthost::ConnectionId { .endpoint = "tmux-compat", .index = 1 }, std::move(first.server)));
    loop.spawn(
        handler(vthost::ConnectionId { .endpoint = "tmux-compat", .index = 2 }, std::move(second.server)));
    std::ignore = loop.runUntilIdle();
    CHECK(registry.size() == 2);
    CHECK(connections.ended == 0);

    registry.closeAll();
    std::ignore = loop.runUntilIdle();
    CHECK(connections.ended == 2);
    CHECK(registry.size() == 0);
}

TEST_CASE("a connection that ends of itself leaves the registry", "[vthost][connections]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto connections = ReadingConnections {};
    auto const registry = vthost::ConnectionRegistry {};
    auto const handler = registry.tracked(connections.handler());

    auto pair = core::net::testing::InMemorySocketPair::create();
    loop.spawn(
        handler(vthost::ConnectionId { .endpoint = "tmux-compat", .index = 1 }, std::move(pair.server)));
    std::ignore = loop.runUntilIdle();
    REQUIRE(registry.size() == 1);

    pair.client->close();
    std::ignore = loop.runUntilIdle();
    CHECK(connections.ended == 1);
    CHECK(registry.size() == 0);

    // Nothing left to close.
    registry.closeAll();
}
