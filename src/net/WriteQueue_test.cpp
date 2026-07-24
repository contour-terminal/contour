// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>

#include <coro/Task.hpp>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/WriteQueue.h>
#include <net/testing/InMemoryTransport.h>

using coro::Task;
using net::EventLoop;
using net::WriteQueue;

namespace
{

/// Reads from @p sock until @p expectedSize bytes arrived (or EOF/error).
Task<void> collectBytes(net::ISocket* sock, std::size_t expectedSize, std::string* out)
{
    auto buffer = std::array<std::byte, 256> {};
    while (out->size() < expectedSize)
    {
        auto const got = co_await sock->read(buffer);
        if (!got.has_value() || *got == 0)
            co_return;
        out->append(reinterpret_cast<char const*>(buffer.data()), *got);
    }
}

/// A trivial root flow so blockOn pumps the loop until spawned drains settle.
Task<void> settle(EventLoop* loop)
{
    co_await loop->delay(std::chrono::milliseconds { 0 });
}

} // namespace

TEST_CASE("WriteQueue drains frames in FIFO order, each frame atomically", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto queue = WriteQueue { loop, pair->first.get(), 1024 };

    // Two logical producers enqueue interleaved: the wire must carry whole
    // frames in enqueue order — never bytes of one frame inside another.
    REQUIRE(queue.enqueue("AAAA"));
    REQUIRE(queue.enqueue("bb"));
    REQUIRE(queue.enqueue("CCCC"));
    REQUIRE(queue.queuedBytes() == 10);

    auto received = std::string {};
    loop.blockOn(collectBytes(pair->second.get(), 10, &received));

    REQUIRE(received == "AAAAbbCCCC");
    REQUIRE(queue.queuedBytes() == 0);
}

TEST_CASE("WriteQueue rejects frames beyond its byte bound", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto queue = WriteQueue { loop, pair->first.get(), 10 };

    REQUIRE(queue.enqueue(std::string(6, 'a')));
    REQUIRE_FALSE(queue.enqueue(std::string(6, 'b'))); // 12 > 10: rejected
    REQUIRE(queue.enqueue(std::string(4, 'c')));       // 10 == 10: fits exactly

    auto received = std::string {};
    loop.blockOn(collectBytes(pair->second.get(), 10, &received));
    REQUIRE(received == "aaaaaacccc"); // the rejected frame left no trace
}

TEST_CASE("A write failure poisons the queue and drops the backlog", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    // Destroy the receiving end: the next drain write hits a dead peer.
    pair->second->close();

    auto queue = WriteQueue { loop, pair->first.get(), 1024 };
    REQUIRE(queue.enqueue("doomed"));
    REQUIRE(queue.enqueue("also doomed"));

    loop.blockOn(settle(&loop));
    loop.blockOn(settle(&loop)); // second pump: the drain observed the failure

    REQUIRE(queue.failure().has_value());
    REQUIRE(queue.queuedBytes() == 0);           // backlog dropped
    REQUIRE_FALSE(queue.enqueue("after error")); // poisoned
}

TEST_CASE("close() drops queued frames and refuses new ones", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto queue = WriteQueue { loop, pair->first.get(), 1024 };
    REQUIRE(queue.enqueue("never sent"));
    queue.close();

    REQUIRE(queue.queuedBytes() == 0);
    REQUIRE_FALSE(queue.enqueue("rejected"));

    loop.blockOn(settle(&loop)); // the spawned drain sees the closed queue and exits
    REQUIRE_FALSE(queue.draining());
}
