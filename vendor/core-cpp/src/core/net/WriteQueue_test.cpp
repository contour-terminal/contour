// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/WriteQueue.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using core::async::Task;
using core::net::EventLoop;
using core::net::WriteQueue;
using namespace std::chrono_literals;

namespace
{

/// Reads from @p sock until @p expectedSize bytes arrived (or EOF/error).
Task<void> collectBytes(core::net::ISocket* sock, std::size_t expectedSize, std::string* out)
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
class ParkingSocket: public core::net::ISocket
{
  public:
    /// @param loop The loop a parked write polls on (not owned; outlives this socket).
    explicit ParkingSocket(EventLoop& loop) noexcept: _loop { loop } {}

    /// Lets the parked write — and every write after it — complete.
    void release() noexcept { _isParked = false; }

    /// @return Every byte written so far, in write order.
    [[nodiscard]] std::string const& written() const noexcept { return _written; }

    /// Never exercised: a WriteQueue only ever writes.
    /// @return A clean EOF, with no wait.
    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> /*buffer*/) override
    {
        return core::net::IoAwaitable { core::net::IoResult { std::size_t { 0 } } };
    }

    /// Coroutine-backed: this double PARKS on a predicate, which is what makes it a test of
    /// backpressure rather than of a buffer copy.
    /// @param buffer The source.
    /// @return The byte count once released, or BadHandle if closed while parked.
    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return core::net::IoAwaitable { writeWhenReleased(buffer) };
    }

  private:
    Task<core::net::IoResult> writeWhenReleased(std::span<std::byte const> buffer)
    {
        co_await core::net::pollUntil(&_loop, [this] { return !_isParked || _isClosed; });
        if (_isClosed)
            co_return std::unexpected(
                core::net::makeNetError(core::net::NetErrorCode::BadHandle, 0, "write on closed socket"));
        _written.append(reinterpret_cast<char const*>(buffer.data()), buffer.size());
        co_return buffer.size();
    }

  public:
    void close() noexcept override { _isClosed = true; }
    [[nodiscard]] bool isClosed() const noexcept override { return _isClosed; }

  private:
    EventLoop& _loop;      ///< Drives the poll a parked write suspends on.
    std::string _written;  ///< Everything that made it through, in order.
    bool _isParked = true; ///< Writes suspend until release() clears this.
    bool _isClosed = false;
};

/// An @c ISocket that parks every write until the test grants it a permit, and records how many
/// writes it was holding at once.
///
/// **What it watches is the contract's write slot, from the outside.** A socket has ONE write
/// operation, and a second one armed over a parked one drops it (@c contract::claimWriteSlot). A
/// reactor socket only asserts that in Debug and only when both writes reach the SAME socket's
/// slot, so the queue's single-writer promise needs an observer that counts rather than aborts --
/// one that turns a second writer into a failed assertion in a case, on every build.
///
/// Every write parks, not just the first, because the interleaving that would expose a second
/// writer is a producer enqueuing while a write is PARKED -- which `ParkingSocket` offers only once.
class PermitSocket: public core::net::ISocket
{
  public:
    /// @param loop The loop a parked write polls on (not owned; outlives this socket).
    explicit PermitSocket(EventLoop& loop) noexcept: _loop { loop } {}

    /// Lets exactly one parked (or future) write complete.
    void grant() noexcept { ++_permits; }

    /// @return Each write's bytes, one entry per `write` call, in call order.
    [[nodiscard]] std::vector<std::string> const& writes() const noexcept { return _writes; }

    /// @return The most writes this socket was ever holding at once. The contract allows one.
    [[nodiscard]] int maxInFlight() const noexcept { return _maxInFlight; }

    /// @return How many writes are parked right now.
    [[nodiscard]] int inFlight() const noexcept { return _inFlight; }

    /// Never exercised: a WriteQueue only ever writes.
    /// @return A clean EOF, with no wait.
    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> /*buffer*/) override
    {
        return core::net::IoAwaitable { core::net::IoResult { std::size_t { 0 } } };
    }

    /// @param buffer The source.
    /// @return The byte count once a permit is granted, or BadHandle if closed while parked.
    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return core::net::IoAwaitable { writeOnPermit(buffer) };
    }

    void close() noexcept override { _isClosed = true; }
    [[nodiscard]] bool isClosed() const noexcept override { return _isClosed; }

  private:
    Task<core::net::IoResult> writeOnPermit(std::span<std::byte const> buffer)
    {
        ++_inFlight;
        _maxInFlight = std::max(_maxInFlight, _inFlight);
        co_await core::net::pollUntil(&_loop, [this] { return _permits > 0 || _isClosed; });
        --_inFlight;
        if (_isClosed)
            co_return std::unexpected(
                core::net::makeNetError(core::net::NetErrorCode::BadHandle, 0, "write on closed socket"));
        --_permits;
        _writes.emplace_back(reinterpret_cast<char const*>(buffer.data()), buffer.size());
        co_return buffer.size();
    }

    EventLoop& _loop;                 ///< Drives the poll a parked write suspends on.
    std::vector<std::string> _writes; ///< One entry per completed write.
    int _permits = 0;                 ///< Writes allowed to complete.
    int _inFlight = 0;                ///< Writes parked right now.
    int _maxInFlight = 0;             ///< The high-water mark of `_inFlight`.
    bool _isClosed = false;
};

/// Waits for room on @p queue and records that it resumed.
///
/// A free coroutine taking pointers, not a capturing lambda: `spawn` outlives the expression
/// that built it, so a closure's captures would dangle (the same reason serveNativeClient is
/// free in vthost).
Task<void> awaitRoom(WriteQueue* queue, std::size_t watermark, bool* resumed)
{
    co_await queue->waitUntilBacklogBelow(watermark);
    *resumed = true;
}

/// Parks @p queue over a watermark of 64: one frame in flight on a socket that will not take it,
/// and a bigger one behind it. The 16/128/64 relationship is what every waiter test depends on,
/// so it is stated once.
void stallOverWatermark(EventLoop& loop, WriteQueue& queue)
{
    REQUIRE(queue.enqueue(std::string(16, 'x')));
    loop.blockOn(core::net::testing::sleepFor(&loop, 20ms));
    REQUIRE(queue.enqueue(std::string(128, 'y')));
    REQUIRE(queue.backlogBytes() == 128);
}

/// Waits for room, or for @p callerDone — the shape a connection tearing down needs.
Task<void> awaitRoomOrDone(WriteQueue* queue, std::size_t watermark, bool const* callerDone, bool* resumed)
{
    co_await queue->waitUntilBacklogBelow(watermark, [callerDone] { return *callerDone; });
    *resumed = true;
}

} // namespace

TEST_CASE("WriteQueue drains frames in FIFO order, each frame atomically", "[net][writequeue]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
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

TEST_CASE("WriteQueue never has two writes in flight, and writes one frame per write", "[net][writequeue]")
{
    // The queue exists to serialise writes over a socket that has ONE write operation. Frames are
    // enqueued in every state the drain can be in -- before it starts, while its write is parked,
    // between two writes, and after it has finished and a new burst must start a new drain -- and
    // at no point may the socket be holding two writes, nor may a frame be split or merged.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto socket = PermitSocket { loop };
    auto queue = WriteQueue { loop, &socket, 1024 };

    auto const settleUntil = [&](auto predicate) {
        REQUIRE(loop.blockOn(core::net::testing::waitUntil(&loop, predicate)));
    };

    REQUIRE(queue.enqueue("one"));
    REQUIRE(queue.enqueue("two"));
    settleUntil([&] { return socket.inFlight() > 0; }); // the drain's first write is parked

    REQUIRE(queue.enqueue("three")); // lands while a write is parked
    socket.grant();
    settleUntil([&] { return socket.writes().size() == 1 && socket.inFlight() > 0; });

    REQUIRE(queue.enqueue("four")); // lands between two writes of the same drain
    socket.grant();
    socket.grant();
    socket.grant();
    settleUntil([&] { return !queue.draining(); }); // the burst is out and the drain has ended

    REQUIRE(queue.enqueue("five")); // a new burst: a new drain, never a second one
    REQUIRE(queue.enqueue("six"));
    socket.grant();
    socket.grant();
    settleUntil([&] { return !queue.draining(); });

    CHECK(socket.maxInFlight() == 1);
    CHECK(socket.writes() == std::vector<std::string> { "one", "two", "three", "four", "five", "six" });
}

TEST_CASE("WriteQueue rejects frames that push the backlog beyond its byte bound", "[net][writequeue]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

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
    loop.blockOn(core::net::testing::sleepFor(&loop, 20ms));

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
    REQUIRE(loop.blockOn(core::net::testing::waitUntil(&loop, [&] { return !queue.draining(); })));
    REQUIRE(queue.queuedBytes() == 0);
    REQUIRE(socket.written().size() == InFlightFrame + 32);
    // The refused frame left no trace, and the accepted one followed the in-flight frame whole.
    REQUIRE(socket.written().substr(InFlightFrame) == std::string(32, 'a'));
}

TEST_CASE("dropTagged discards superseded frames only", "[net][writequeue]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto queue = WriteQueue { loop, pair->first.get(), 1024 };
    REQUIRE(queue.enqueue("never sent"));
    queue.close();

    REQUIRE(queue.queuedBytes() == 0);
    REQUIRE_FALSE(queue.enqueue("rejected"));

    loop.blockOn(settle(&loop)); // the spawned drain sees the closed queue and exits
    REQUIRE_FALSE(queue.draining());
}

TEST_CASE("waitUntilBacklogBelow returns without parking when there is already room", "[net][writequeue]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto queue = WriteQueue { loop, pair->first.get(), 1024 };

    // The pacing wait must cost nothing in the overwhelmingly common case, or a producer that
    // consults it between every frame pays a loop tick per frame for no reason.
    auto resumed = false;
    loop.blockOn(awaitRoom(&queue, 64, &resumed));
    REQUIRE(resumed);
}

TEST_CASE("waitUntilBacklogBelow parks until the drain makes room", "[net][writequeue]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    // Declared before the queue so it outlives the drain that writes to it.
    auto socket = ParkingSocket { loop };
    auto queue = WriteQueue { loop, &socket, 1024 };

    // One frame in flight (the socket parks it) and a bigger one behind it, so the backlog
    // sits above the watermark until the peer catches up — exactly the state a producer
    // emitting faster than one loop turn can drain puts the queue in.
    stallOverWatermark(loop, queue);

    auto resumed = false;
    loop.spawn(awaitRoom(&queue, 64, &resumed));
    loop.blockOn(core::net::testing::sleepFor(&loop, 20ms));
    REQUIRE_FALSE(resumed); // parked: the backlog is over the mark

    // The peer catches up; the backlog drains and the waiter resumes on its own.
    socket.release();
    REQUIRE(loop.blockOn(core::net::testing::waitUntil(&loop, [&] { return resumed; })));
    REQUIRE(queue.backlogBytes() == 0);
}

TEST_CASE("waitUntilBacklogBelow stops waiting once the queue is closed", "[net][writequeue]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto socket = ParkingSocket { loop };
    auto queue = WriteQueue { loop, &socket, 1024 };

    stallOverWatermark(loop, queue);

    auto resumed = false;
    loop.spawn(awaitRoom(&queue, 64, &resumed));
    loop.blockOn(core::net::testing::sleepFor(&loop, 20ms));
    REQUIRE_FALSE(resumed);

    // A connection being torn down must not strand its pacer: an owner that closes the queue
    // and then polls for its producers to finish would otherwise deadlock, because a closed
    // queue never drains again and the watermark can never be met.
    queue.close();
    REQUIRE(loop.blockOn(core::net::testing::waitUntil(&loop, [&] { return resumed; })));
}

TEST_CASE("waitUntilBacklogBelow stops waiting when the caller says it is done", "[net][writequeue]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto socket = ParkingSocket { loop };
    auto queue = WriteQueue { loop, &socket, 1024 };

    stallOverWatermark(loop, queue);

    auto callerDone = false;
    auto resumed = false;
    loop.spawn(awaitRoomOrDone(&queue, 64, &callerDone, &resumed));
    loop.blockOn(core::net::testing::sleepFor(&loop, 20ms));
    REQUIRE_FALSE(resumed);

    // The close() the queue-state predicate watches for is not available to a connection tearing
    // down: it closes the queue only AFTER waiting for its producers to let go, so a producer
    // waiting on the close would be waiting for something waiting on it. The caller's own flag is
    // what breaks that cycle, and it must work with the backlog still stuck and the queue open.
    callerDone = true;
    REQUIRE(loop.blockOn(core::net::testing::waitUntil(&loop, [&] { return resumed; })));
    CHECK(queue.backlogBytes() == 128); // still stuck: the caller left, the peer did not catch up

    // Leave no coroutine parked on the socket at teardown.
    socket.release();
    REQUIRE(loop.blockOn(core::net::testing::waitUntil(&loop, [&] { return !queue.draining(); })));
}

TEST_CASE("A WriteQueue refuses a null socket at construction", "[net][writequeue]")
{
    // `ITlsContext::wrap` returns null when it cannot allocate. A queue built on that null
    // used to construct cleanly and crash in `close()` — which is `noexcept`, so the failure
    // surfaced at teardown rather than at the call that caused it. A constructed object is
    // usable, so the refusal belongs at construction, where the caller can still act on it.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    REQUIRE_THROWS_AS((WriteQueue { loop, nullptr, 1024 }), std::invalid_argument);
}
