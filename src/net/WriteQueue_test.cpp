// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string>

#include <coro/Task.hpp>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/IoResult.h>
#include <net/PollEventSource.h>
#include <net/WriteQueue.h>
#include <net/testing/CoroTestSupport.h>
#include <net/testing/InMemoryTransport.h>

using coro::Task;
using net::EventLoop;
using net::WriteQueue;
using namespace std::chrono_literals;

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

/// An @c ISocket whose writes park until the test releases them.
///
/// Backpressure is a property of the TRANSPORT, and no frame size produces it portably: a POSIX
/// socketpair parks on a megabyte, while a Windows loopback pair (what @c makeSocketPair builds
/// there) buffers the whole thing and completes the write. Asking a real pair to park therefore
/// asserts an accident of the platform rather than anything about the queue. Parking on demand
/// states the precondition instead of hoping for it.
class ParkingSocket: public net::ISocket
{
  public:
    /// @param loop The loop a parked write polls on (not owned; outlives this socket).
    explicit ParkingSocket(EventLoop& loop) noexcept: _loop { loop } {}

    /// Lets the parked write — and every write after it — complete.
    void release() noexcept { _isParked = false; }

    /// @return Every byte written so far, in write order.
    [[nodiscard]] std::string const& written() const noexcept { return _written; }

    /// Never exercised: a WriteQueue only ever writes.
    [[nodiscard]] Task<net::IoResult> read(std::span<std::byte> /*buffer*/) override
    {
        co_return std::size_t { 0 }; // clean EOF
    }

    [[nodiscard]] Task<net::IoResult> write(std::span<std::byte const> buffer) override
    {
        co_await net::pollUntil(&_loop, [this] { return !_isParked || _isClosed; });
        if (_isClosed)
            co_return std::unexpected(
                net::makeNetError(net::NetErrorCode::BadHandle, 0, "write on closed socket"));
        _written.append(reinterpret_cast<char const*>(buffer.data()), buffer.size());
        co_return buffer.size();
    }

    void close() noexcept override { _isClosed = true; }
    [[nodiscard]] bool isClosed() const noexcept override { return _isClosed; }

  private:
    EventLoop& _loop;      ///< Drives the poll a parked write suspends on.
    std::string _written;  ///< Everything that made it through, in order.
    bool _isParked = true; ///< Writes suspend until release() clears this.
    bool _isClosed = false;
};

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

TEST_CASE("WriteQueue rejects frames that push the backlog beyond its byte bound", "[net][writequeue]")
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

TEST_CASE("A frame is never refused for its own size alone", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    // The bound exists to detect a peer that stopped draining. A frame landing on an
    // EMPTY backlog says nothing about the peer, so its size alone is never grounds for
    // refusal — otherwise a legitimately large message (a full-grid snapshot of a deep
    // scrollback) can never be sent, a cliff neither producer nor peer can avoid.
    auto queue = WriteQueue { loop, pair->first.get(), 10 };
    REQUIRE(queue.enqueue(std::string(64, 'x'))); // 64 > 10, yet accepted

    auto received = std::string {};
    loop.blockOn(collectBytes(pair->second.get(), 64, &received));
    REQUIRE(received == std::string(64, 'x'));
}

TEST_CASE("The bound governs the backlog; the in-flight frame is still owed", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    // Declared before the queue so it outlives the drain that writes to it.
    auto socket = ParkingSocket { loop };
    auto queue = WriteQueue { loop, &socket, 64 };

    // The socket parks this write, so the frame stays in flight for the rest of the test —
    // which is the state every assertion below is about.
    auto constexpr InFlightFrame = std::size_t { 1024 };
    REQUIRE(queue.enqueue(std::string(InFlightFrame, 'x')));
    // A real delay, not `settle` and not a waitUntil predicate: pumpOnce decides whether to
    // block AFTER running the ready queue, so a root flow that finishes without leaving a
    // timer pending lets poll() wait forever on the drain parked on this socket. Only a timer
    // that outlives the pump bounds it.
    loop.blockOn(net::testing::sleepFor(&loop, 20ms));

    REQUIRE(queue.draining());
    REQUIRE(queue.backlogBytes() == 0);            // it left the backlog...
    REQUIRE(queue.queuedBytes() == InFlightFrame); // ...but is still owed to the peer

    // The backlog is empty, so the next frame is accepted however big the in-flight one is.
    REQUIRE(queue.enqueue(std::string(32, 'a')));
    // Now a backlog exists, and a frame that would push it past the bound is refused —
    // this is the only situation that actually evidences a peer falling behind.
    REQUIRE_FALSE(queue.enqueue(std::string(64, 'b')));
    REQUIRE(queue.describeRefusal().contains("in flight"));

    // Let the peer catch up: the parked write completes and the accepted frame follows,
    // which also leaves no coroutine parked on the socket at teardown.
    socket.release();
    REQUIRE(loop.blockOn(net::testing::waitUntil(&loop, [&] { return !queue.draining(); })));
    REQUIRE(queue.queuedBytes() == 0);
    REQUIRE(socket.written().size() == InFlightFrame + 32);
    // The refused frame left no trace, and the accepted one followed the in-flight frame whole.
    REQUIRE(socket.written().substr(InFlightFrame) == std::string(32, 'a'));
}

TEST_CASE("dropTagged discards superseded frames only", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto queue = WriteQueue { loop, pair->first.get(), 1024 };

    REQUIRE(queue.enqueue("hello", 0)); // untagged: a handshake, never superseded
    REQUIRE(queue.enqueue("A1", 1));
    REQUIRE(queue.enqueue("B1", 2));
    REQUIRE(queue.enqueue("A2", 1));
    REQUIRE(queue.queuedBytes() == 11);

    // A new frame for tag 1 fully re-describes what its earlier frames said.
    CHECK(queue.dropTagged(1) == 2);   // reported, so a caller can skip rebuilding what it sent
    REQUIRE(queue.queuedBytes() == 7); // "hello" + "B1"
    REQUIRE(queue.enqueue("A3", 1));

    auto received = std::string {};
    loop.blockOn(collectBytes(pair->second.get(), 9, &received));
    // Tag 2 and the untagged frame kept both their content and their relative order.
    // Spelled as a concatenation rather than one literal: a glued "helloB1A3" reads to the
    // spell gate as a novel word needing an expect.txt entry (@see this file's "aaaaaacccc").
    REQUIRE(received == std::string { "hello" } + "B1" + "A3");
}

TEST_CASE("dropTagged never discards untagged frames", "[net][writequeue]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto queue = WriteQueue { loop, pair->first.get(), 1024 };

    REQUIRE(queue.enqueue("keep me")); // tag 0 by default
    CHECK(queue.dropTagged(0) == 0);   // the untagged label matches nothing

    REQUIRE(queue.queuedBytes() == 7);

    auto received = std::string {};
    loop.blockOn(collectBytes(pair->second.get(), 7, &received));
    REQUIRE(received == "keep me");
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
