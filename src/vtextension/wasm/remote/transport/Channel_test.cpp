// SPDX-License-Identifier: Apache-2.0
//
// The byte channel between Contour and its extension host process: both ways, closing, and what a child
// process does not inherit.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <Windows.h>
#else
    #include <csignal>

    #include <fcntl.h>
#endif

using vtextension::wasm::remote::transport::Channel;
using vtextension::wasm::remote::transport::ChannelError;
using vtextension::wasm::remote::transport::makeChannelPair;

namespace
{

/// @return Exactly @p count bytes received off @p channel; nothing if it failed first.
[[nodiscard]] std::optional<std::vector<uint8_t>> receiveExactly(Channel& channel, std::size_t count)
{
    auto received = std::vector<uint8_t>(count);
    auto filled = std::size_t { 0 };
    while (filled < count)
    {
        auto const some = channel.receiveSome(std::span { received }.subspan(filled));
        if (!some)
            return std::nullopt;
        filled += *some;
    }
    return received;
}

} // namespace

TEST_CASE("Channel.pairCarriesBytesBothWays", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());

    auto const request = std::vector<uint8_t> { 1, 2, 3, 4, 5 };
    REQUIRE(pair->parent.send(request).has_value());
    CHECK(receiveExactly(pair->helper, request.size()) == request);

    // Larger than any socket buffer: the sender blocks until the receiver drains it.
    auto const reply = std::vector<uint8_t>(std::size_t { 4 } * 1024 * 1024, uint8_t { 0x5a });
    auto sent = std::async(std::launch::async, [&] { return pair->helper.send(reply); });
    CHECK(receiveExactly(pair->parent, reply.size()) == reply);
    CHECK(sent.get().has_value());
}

TEST_CASE("Channel.shutdownUnblocksReceive", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());

    auto result = std::promise<std::expected<std::size_t, ChannelError>> {};
    auto received = result.get_future();
    auto reader = std::jthread { [&] {
        auto buffer = std::array<uint8_t, 16> {};
        result.set_value(pair->parent.receiveSome(buffer));
    } };
    // Whether the reader blocked before the shutdown or arrives after it, it returns Closed.
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    pair->parent.shutdown();
    CHECK(received.get() == std::unexpected(ChannelError::Closed));

    // And so does every later receive and send on that end.
    auto buffer = std::array<uint8_t, 16> {};
    CHECK(pair->parent.receiveSome(buffer) == std::unexpected(ChannelError::Closed));
    CHECK(pair->parent.send(std::vector<uint8_t> { 1 }) == std::unexpected(ChannelError::Closed));
}

TEST_CASE("Channel.shutdownEndsThePeersStream", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());

    auto result = std::promise<std::expected<std::size_t, ChannelError>> {};
    auto received = result.get_future();
    auto peer = std::jthread { [&] {
        auto buffer = std::array<uint8_t, 16> {};
        result.set_value(pair->helper.receiveSome(buffer));
    } };
    // Whether the peer blocked before the shutdown or arrives after it, its stream has ended.
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    pair->parent.shutdown();
    CHECK(received.get() == std::unexpected(ChannelError::Closed));
    CHECK(pair->helper.send(std::vector<uint8_t>(1024, uint8_t { 1 }))
          == std::unexpected(ChannelError::Closed));
}

TEST_CASE("Channel.peerCloseEndsTheStream", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    {
        auto const helper = std::move(pair->helper);
    }
    auto buffer = std::array<uint8_t, 16> {};
    CHECK(pair->parent.receiveSome(buffer) == std::unexpected(ChannelError::Closed));
}

TEST_CASE("Channel.sendToClosedPeerFails", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
#ifndef _WIN32
    struct sigaction before {};
    REQUIRE(::sigaction(SIGPIPE, nullptr, &before) == 0);
#endif
    {
        auto const helper = std::move(pair->helper);
    }
    CHECK(pair->parent.send(std::vector<uint8_t>(1024, uint8_t { 1 }))
          == std::unexpected(ChannelError::Closed));
#ifndef _WIN32
    // The process survived the write without a SIGPIPE, and without anyone having to ignore the signal.
    struct sigaction after {};
    REQUIRE(::sigaction(SIGPIPE, nullptr, &after) == 0);
    CHECK(after.sa_handler == before.sa_handler);
#endif
}

#ifndef _WIN32
TEST_CASE("Channel.endsAreCloseOnExec", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    for (auto const* channel: { &pair->parent, &pair->helper })
    {
        auto const flags = ::fcntl(channel->nativeHandle(), F_GETFD);
        REQUIRE(flags != -1);
        CHECK((flags & FD_CLOEXEC) != 0);
    }
}
#endif

#ifdef _WIN32
TEST_CASE("Channel.endsAreNotInheritable", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    for (auto const* channel: { &pair->parent, &pair->helper })
    {
        auto flags = DWORD { 0 };
        REQUIRE(::GetHandleInformation(channel->nativeHandle(), &flags));
        CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
    }
}
#endif
