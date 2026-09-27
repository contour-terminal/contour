// SPDX-License-Identifier: Apache-2.0
//
// What a socket's park must still answer once the loop keeps it across operations
// (core-cpp#52).
//
// A socket files a frameless read or write park each time an operation has to wait, and takes it
// when the operation completes. The loop keeps that park's storage for the socket's next operation
// rather than filing and taking a fresh one. These cases pin what reuse must NOT change, each at
// the loop's own interface with a real descriptor on every backend the platform has:
//
// - a cancel for an operation that has finished -- delivered a turn late from another thread --
//   finds nothing, even though the next operation is parked in the same storage;
// - a readiness entry queued for an operation that is retired before the drain reaches it runs
//   nothing, even though the next operation was filed in its place within the same drain;
// - a park with no operation hears nothing and is not counted, and the registration still narrows
//   away readability that nobody reads, so a level-triggered backend does not report it every wait;
// - a descriptor closed and its number reused names none of the old ids.
//
// Each held for the park table the loop had before reuse, where every operation got a park and an
// id of its own; they are here so the reuse is held to the same answers. POSIX-only for the raw
// `socketpair`: the loop interface is the property, and a descriptor is the cheapest way to hold it.
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/detail/WouldBlock.hpp>
#include <core/net/testing/BackendMatrix.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <ranges>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using core::net::EventLoop;
using core::net::Interest;
using core::net::ParkEntry;
using core::net::ParkId;
using core::net::ParkWake;
using core::net::RegistrationLifetime;
using core::net::testing::BackendMatrix;

namespace
{

/// What a frameless park's owner was told, per reason.
struct Wakes
{
    std::size_t ready = 0;
    std::size_t cancelled = 0;
    std::size_t abandoned = 0;
};

/// Counts a wake into the @c Wakes it is handed.
void countWake(void* state, ParkWake wake)
{
    auto& wakes = *static_cast<Wakes*>(state);
    switch (wake)
    {
        case ParkWake::Ready: ++wakes.ready; return;
        case ParkWake::Cancelled: ++wakes.cancelled; return;
        case ParkWake::Abandoned: ++wakes.abandoned; return;
    }
}

/// A connected pair of descriptors, closed on the way out.
class DescriptorPair
{
  public:
    DescriptorPair(): _ok(::socketpair(AF_UNIX, SOCK_STREAM, 0, _fds.data()) == 0) {}
    DescriptorPair(DescriptorPair const&) = delete;
    DescriptorPair(DescriptorPair&&) = delete;
    DescriptorPair& operator=(DescriptorPair const&) = delete;
    DescriptorPair& operator=(DescriptorPair&&) = delete;
    ~DescriptorPair() { closeBoth(); }

    [[nodiscard]] bool ok() const noexcept { return _ok; }
    [[nodiscard]] int local() const noexcept { return _fds[0]; }

    /// Makes @c local readable: one byte from the other end, never read.
    void sendByte() const
    {
        auto const byte = std::array<char, 1> { 'x' };
        REQUIRE(::write(_fds[1], byte.data(), byte.size()) == 1);
    }

    /// Fills @c local's send buffer, so it is not writable until @c drainPeer.
    void fillSendBuffer() const
    {
        REQUIRE(::fcntl(_fds[0], F_SETFL, ::fcntl(_fds[0], F_GETFL) | O_NONBLOCK) == 0);
        auto const chunk = std::array<char, 4096> {};
        while (::write(_fds[0], chunk.data(), chunk.size()) > 0)
        {
        }
        REQUIRE(core::net::detail::isWouldBlock(errno));
    }

    /// Reads everything the other end holds, so @c local is writable again.
    void drainPeer() const
    {
        REQUIRE(::fcntl(_fds[1], F_SETFL, ::fcntl(_fds[1], F_GETFL) | O_NONBLOCK) == 0);
        auto chunk = std::array<char, 4096> {};
        while (::read(_fds[1], chunk.data(), chunk.size()) > 0)
        {
        }
    }

    /// Closes both ends, announcing @c local to @p loop first as a socket's close does.
    void close(EventLoop& loop)
    {
        loop.notifyHandleClosing(_fds[0], core::net::FdWakePolicy::Resume);
        closeBoth();
    }

  private:
    void closeBoth() noexcept
    {
        for (auto& fd: _fds)
            if (fd >= 0)
                std::ignore = ::close(std::exchange(fd, -1));
    }

    std::array<int, 2> _fds { -1, -1 };
    bool _ok = false;
};

/// Files a frameless park on @p fd for @p interest, on the registration kept for the handle's life,
/// as `PosixSocket::armRead` and `armWrite` do.
/// @return Its id.
ParkId fileOperation(EventLoop& loop, int fd, Interest interest, Wakes* wakes)
{
    auto const park = loop.registerPark(ParkEntry::onReadyCallback(
        &countWake, wakes, fd, core::net::DefaultHandleKind, interest, RegistrationLifetime::UntilClosed));
    REQUIRE(park);
    return park;
}

/// Turns @p loop a few times without waiting, as a drain would.
void turnFewTimes(EventLoop& loop)
{
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 3))
        std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
}

} // namespace

TEST_CASE("A late cancel for a finished operation does not reach the socket's next one",
          "[net][loop][parkreuse]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = DescriptorPair {};
            REQUIRE(pair.ok());

            auto first = Wakes {};
            auto const finished = fileOperation(loop, pair.local(), Interest::Read, &first);
            loop.unregisterPark(finished); // the operation completed
            auto next = Wakes {};
            auto const current = fileOperation(loop, pair.local(), Interest::Read, &next);
            CHECK(current != finished);

            // The stop callback of the finished operation's flow, on another thread, after its park
            // was taken: it reaches the loop through the inbound queue and is resolved next turn.
            auto stopper = std::thread { [&loop, finished] { loop.requestCancel(finished); } };
            stopper.join();
            turnFewTimes(loop);

            CHECK(next.cancelled == 0);
            CHECK(first.cancelled == 0);
            CHECK(loop.parkedWaiterCount() == 1);

            // The current operation is still cancellable by its own id.
            loop.requestCancel(current);
            turnFewTimes(loop);
            CHECK(next.cancelled == 1);
            loop.unregisterPark(current);
            CHECK(loop.parkedWaiterCount() == 0);
            pair.close(loop);
        }
    }
}

namespace
{

/// A read whose completion retires the socket's parked write and files the next one in its place,
/// inside the drain: a reply's read completing, and the flow writing again, as a server does.
struct RetireWriteOnRead
{
    EventLoop* loop = nullptr;
    int fd = -1;
    ParkId write {};       ///< The write this read retires.
    Wakes* nextWrite = {}; ///< What the write filed in its place hears.
    ParkId filed {};       ///< That write's id.
    std::size_t reads = 0; ///< How often this read was woken.
};

void retireWriteOnRead(void* state, ParkWake wake)
{
    auto& self = *static_cast<RetireWriteOnRead*>(state);
    if (wake != ParkWake::Ready || self.reads++ != 0)
        return;
    self.loop->unregisterPark(self.write);
    self.filed = fileOperation(*self.loop, self.fd, Interest::Write, self.nextWrite);
}

} // namespace

TEST_CASE("A ready entry queued for a retired operation does not run the one filed after it",
          "[net][loop][parkreuse]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = DescriptorPair {};
            REQUIRE(pair.ok());

            // The write waits on a full send buffer, so no wait reports it. The read waits for a
            // byte, which the first turn's wait reports and queues.
            pair.fillSendBuffer();
            auto staleWrite = Wakes {};
            auto nextWrite = Wakes {};
            auto reader =
                RetireWriteOnRead { .loop = &loop, .fd = pair.local(), .write = {}, .nextWrite = &nextWrite };
            reader.write = fileOperation(loop, pair.local(), Interest::Write, &staleWrite);
            auto const read =
                loop.registerPark(ParkEntry::onReadyCallback(&retireWriteOnRead,
                                                             &reader,
                                                             pair.local(),
                                                             core::net::DefaultHandleKind,
                                                             Interest::Read,
                                                             RegistrationLifetime::UntilClosed));
            REQUIRE(read);
            pair.sendByte();
            std::ignore = loop.runOnce(std::chrono::milliseconds { 100 });
            REQUIRE(reader.reads == 0); // queued, not yet run

            // The write's flow is stopped from another thread. The next turn resolves that in step
            // 1, queueing the write's entry BEHIND the read's; the read then retires the write and
            // files the next one before the drain reaches the entry.
            auto stopper = std::thread { [&loop, write = reader.write] { loop.requestCancel(write); } };
            stopper.join();
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });

            REQUIRE(reader.reads == 1); // or the order this case needs never happened
            REQUIRE(reader.filed);
            CHECK(reader.filed != reader.write);
            CHECK(staleWrite.cancelled == 0); // retired before its entry ran
            // The entry queued for the retired write ran nothing: the write filed in its place
            // hears neither that cancel nor readiness it has not had.
            CHECK(nextWrite.cancelled == 0);
            CHECK(nextWrite.ready == 0);

            pair.drainPeer();
            turnFewTimes(loop);
            CHECK(nextWrite.ready >= 1); // and it does hear its own
            CHECK(nextWrite.cancelled == 0);

            loop.unregisterPark(reader.filed);
            loop.unregisterPark(read);
            CHECK(loop.parkedWaiterCount() == 0);
            pair.close(loop);
        }
    }
}

TEST_CASE("A socket with no operation parked hears nothing, is not counted, and stops being reported",
          "[net][loop][parkreuse]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = DescriptorPair {};
            REQUIRE(pair.ok());

            auto finished = Wakes {};
            loop.unregisterPark(fileOperation(loop, pair.local(), Interest::Read, &finished));
            CHECK(loop.parkedWaiterCount() == 0);

            // Unread data on a registration kept armed for reading: the first wait may report it,
            // and finds no park to take it, which narrows the registration. After that nothing is
            // reported however often the loop waits -- a level-triggered backend would otherwise
            // report it on every one.
            pair.sendByte();
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
            auto reported = std::size_t { 0 };
            for ([[maybe_unused]] auto const turn: std::views::iota(0, 5))
                reported += loop.runOnce(std::chrono::milliseconds { 0 }).dispatched;
            CHECK(reported == 0);
            CHECK(finished.ready == 0);
            CHECK(finished.cancelled == 0);

            // And the next operation hears the data that was waiting.
            auto next = Wakes {};
            auto const current = fileOperation(loop, pair.local(), Interest::Read, &next);
            CHECK(loop.parkedWaiterCount() == 1);
            turnFewTimes(loop);
            CHECK(next.ready >= 1);
            CHECK(finished.ready == 0);
            loop.unregisterPark(current);
            pair.close(loop);
        }
    }
}

TEST_CASE("A descriptor closed and its number reused names none of the old ids", "[net][loop][parkreuse]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto seen = std::unordered_set<ParkId> {};
            auto stale = std::vector<ParkId> {};
            auto wakes = Wakes {};
            for ([[maybe_unused]] auto const socket: std::views::iota(0, 4))
            {
                auto pair = DescriptorPair {};
                REQUIRE(pair.ok());
                for ([[maybe_unused]] auto const operation: std::views::iota(0, 8))
                {
                    for (auto const interest: { Interest::Read, Interest::Write })
                    {
                        auto const park = fileOperation(loop, pair.local(), interest, &wakes);
                        CHECK(seen.insert(park).second); // never an id handed out before
                        loop.unregisterPark(park);
                        stale.push_back(park);
                    }
                }
                pair.close(loop);
            }

            // A park filed now, very likely on a descriptor number used above, is reached by none
            // of the old ids: not by a cancel, and not by a second unregister.
            auto pair = DescriptorPair {};
            REQUIRE(pair.ok());
            auto next = Wakes {};
            auto const current = fileOperation(loop, pair.local(), Interest::Read, &next);
            CHECK(!seen.contains(current));
            for (auto const old: stale)
            {
                loop.requestCancel(old);
                loop.unregisterPark(old);
            }
            turnFewTimes(loop);
            CHECK(next.cancelled == 0);
            CHECK(loop.parkedWaiterCount() == 1);
            loop.unregisterPark(current);
            CHECK(loop.parkedWaiterCount() == 0);
            pair.close(loop);
        }
    }
}

TEST_CASE("A socket closed with its operation still parked settles it, and taking it afterwards is clean",
          "[net][loop][parkreuse]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto wakes = Wakes {};
            {
                auto pair = DescriptorPair {};
                REQUIRE(pair.ok());
                // One operation completed first, so the one parked at the close is the second on
                // the socket, and the park it holds is the one the first left behind.
                loop.unregisterPark(fileOperation(loop, pair.local(), Interest::Read, &wakes));
                auto const parked = fileOperation(loop, pair.local(), Interest::Read, &wakes);
                loop.notifyHandleClosing(pair.local(), core::net::FdWakePolicy::Cancel);
                turnFewTimes(loop);
                CHECK(wakes.abandoned == 1);
                CHECK(loop.parkedWaiterCount() == 1); // still filed until its owner takes it
                loop.unregisterPark(parked);
                CHECK(loop.parkedWaiterCount() == 0);
            }

            // The loop is still usable for the next socket, whatever number it gets.
            auto pair = DescriptorPair {};
            REQUIRE(pair.ok());
            auto next = Wakes {};
            auto const current = fileOperation(loop, pair.local(), Interest::Read, &next);
            pair.sendByte();
            turnFewTimes(loop);
            CHECK(next.ready >= 1);
            CHECK(next.abandoned == 0);
            loop.unregisterPark(current);
            pair.close(loop);
        }
    }
}
