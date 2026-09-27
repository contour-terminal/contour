// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ParkingReadableSocket` and `ParkingWritableSocket` — decorators that PARK an operation until the
/// test decides, and count how each parked operation was retired.
///
/// Origin: fastcached `src/tests/SocketDecorator.hpp` (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`),
/// where both live beside `SocketDecorator`.

#include <core/net/IoAwaitable.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketContract.hpp>
#include <core/net/testing/SocketDecorator.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace core::net::testing
{

/// Retires a parked operation the way every transport's `close` does: with
/// @c NetErrorCode::Cancelled, as a value.
///
/// **Call it LAST, with every member already touched.** Completing resumes the awaiting
/// coroutine inline, and a coroutine that owns the socket may destroy it before this returns. A
/// free function, so it cannot reach a member itself; the order around it is the caller's, and
/// both fakes below spell it the same way: detach, forward, count, complete.
/// @param parked The operation, already detached from its socket.
inline void completeCancelled(IoAwaitable& parked) noexcept
{
    parked.complete(std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "retired by the test double")));
}

/// A socket whose @c waitReadable ALWAYS parks, until the test resolves or retires it, and which
/// counts what became of every watch.
///
/// **It models the shared read slot rather than asserting on it.** Arming a read verb while a watch
/// is parked ORPHANS that watch -- dropped, never resumed, never freed -- which is exactly what a
/// socket without the slot tripwire does. @c contract::claimReadSlot would instead abort the
/// process, and its abort names the SLOT rather than the caller, so a case that wants to say WHICH
/// caller double-armed has to be able to observe the orphan and carry on. That is what
/// @c watchesOrphaned is for ([fastcached#663](https://github.com/LASTRADA-Software/fastcached/issues/663)).
///
/// **Retired by @c close and by @c cancelRead, counted apart.** A watch retired by the connection's
/// teardown and one retired by the caller that armed it are opposite answers about
/// [fastcached#710](https://github.com/LASTRADA-Software/fastcached/issues/710), and one counter
/// would render them the same.
///
/// **Why it still exists when `InMemorySocket::waitReadable` parks too.** The in-memory socket's
/// watch resolves the moment its pipe has something to say, and enforces the slot by assertion; this
/// one resolves only when the test says so, and counts the orphan instead of aborting. It is the
/// instrument for a case whose subject is the CALLER's handling of a parked watch.
///
/// Single-threaded, like @c InMemorySocket, and with the same limits: there is no loop to route a
/// flow's stop token through, so a parked watch answers the test, @c cancelRead or @c close -- and
/// those two resume the watching flow INSIDE the call, where a real transport's loop resumes it on
/// a later drain step (G2).
class ParkingReadableSocket final: public SocketDecorator
{
  public:
    /// @param inner The socket reads and writes are forwarded to; must outlive this.
    explicit ParkingReadableSocket(ISocket& inner) noexcept: SocketDecorator { inner } {}

    ParkingReadableSocket(ParkingReadableSocket const&) = delete;
    ParkingReadableSocket(ParkingReadableSocket&&) = delete;
    ParkingReadableSocket& operator=(ParkingReadableSocket const&) = delete;
    ParkingReadableSocket& operator=(ParkingReadableSocket&&) = delete;

    /// Abandons a watch a case left parked: the flow unwinds rather than resuming into a socket
    /// that is gone. Not merely tidiness -- a parked watch never completed is a frame never freed,
    /// which is what these counters measure, and a double that leaked one would report the defect
    /// against a fixed build too.
    ~ParkingReadableSocket() override
    {
        if (auto* const parked = std::exchange(_parked, nullptr); parked != nullptr)
            parked->abandon();
    }

    /// Parks, always: there is no synchronous answer, which is the point.
    /// @return An awaitable @c resolveReadable answers, or that @c cancelRead or @c close retires.
    [[nodiscard]] IoAwaitable waitReadable() override
    {
        claimSlot();
        ++_watchesArmed;
        return IoAwaitable { [](void* owner, IoAwaitable& self) {
                                // Recorded here, never in the verb: the verb's awaitable is returned by
                                // value, so its address is not the one the caller suspends on.
                                static_cast<ParkingReadableSocket*>(owner)->_parked = &self;
                            },
                             &ParkingReadableSocket::retire,
                             this };
    }

    /// Forwards, having first claimed the shared read slot the way a real socket does.
    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override
    {
        claimSlot();
        return SocketDecorator::read(buffer);
    }

    /// Retires a parked watch, completing it LAST.
    void cancelRead() noexcept override
    {
        auto* const parked = std::exchange(_parked, nullptr);
        SocketDecorator::cancelRead();
        if (parked == nullptr)
            return;
        ++_watchesRetiredByCancel;
        completeCancelled(*parked);
    }

    /// Retires a parked watch, completing it LAST.
    void close() noexcept override
    {
        auto* const parked = std::exchange(_parked, nullptr);
        SocketDecorator::close();
        if (parked == nullptr)
            return;
        ++_watchesRetiredByClose;
        completeCancelled(*parked);
    }

    /// Resolves the parked watch as a reactor would when the socket becomes readable.
    /// @param count `0` for EOF, non-zero for bytes pending -- the distinction
    ///        @c ISocket::waitReadable makes contractual.
    void resolveReadable(std::size_t count) noexcept
    {
        auto* const parked = std::exchange(_parked, nullptr);
        if (parked == nullptr)
            return;
        ++_watchesResolved;
        parked->complete(IoResult { count });
    }

    /// @return How many readability watches were armed.
    [[nodiscard]] std::size_t watchesArmed() const noexcept { return _watchesArmed; }

    /// @return How many were dropped by a competing arm: never resumed, never freed.
    [[nodiscard]] std::size_t watchesOrphaned() const noexcept { return _watchesOrphaned; }

    /// @return How many the CALLER retired through @c cancelRead.
    [[nodiscard]] std::size_t watchesRetiredByCancel() const noexcept { return _watchesRetiredByCancel; }

    /// @return How many the connection's teardown retired instead -- a different answer from
    ///         @c watchesRetiredByCancel and not a substitute for it.
    [[nodiscard]] std::size_t watchesRetiredByClose() const noexcept { return _watchesRetiredByClose; }

    /// @return How many resolved as readability rather than being retired.
    [[nodiscard]] std::size_t watchesResolved() const noexcept { return _watchesResolved; }

    /// @return Whether a watch is parked on the read slot right now.
    [[nodiscard]] bool isWatchParked() const noexcept { return _parked != nullptr; }

  private:
    /// Takes the read slot for a new operation, orphaning whatever held it -- which is what a socket
    /// without the tripwire does, and the defect the counter names.
    void claimSlot() noexcept
    {
        if (_parked == nullptr)
            return;
        ++_watchesOrphaned;
        _parked = nullptr;
    }

    /// Forgets a watch whose flow is unwinding.
    static void retire(void* owner, void* awaitable) noexcept
    {
        auto* const socket = static_cast<ParkingReadableSocket*>(owner);
        if (socket->_parked == awaitable)
            socket->_parked = nullptr;
    }

    IoAwaitable* _parked { nullptr };
    std::size_t _watchesArmed { 0 };
    std::size_t _watchesOrphaned { 0 };
    std::size_t _watchesRetiredByCancel { 0 };
    std::size_t _watchesRetiredByClose { 0 };
    std::size_t _watchesResolved { 0 };
};

/// A socket whose writes PARK once a case says the peer has stopped reading.
///
/// A reactor socket facing a peer that stopped reading arms the write and suspends, and the
/// suspended write is what a stall IS. A bound on that suspension -- a hold a push may not outlast --
/// is reachable only through a write that stays parked until something retrieves it; a bounded
/// @c InMemorySocketPair parks too, but resumes the moment its reader drains, which is the opposite
/// of what a stall case needs.
///
/// **Only @c close retrieves a parked write**, which is the transports' own rule: there is no
/// `cancelWrite` (@c contract::claimWriteSlot says why), so a parked write is completed with a
/// failure at @c close and at nothing else. Until @c stopReading, every write is forwarded.
class ParkingWritableSocket final: public SocketDecorator
{
  public:
    /// @param inner The socket reads and unparked writes are forwarded to; must outlive this.
    explicit ParkingWritableSocket(ISocket& inner) noexcept: SocketDecorator { inner } {}

    ParkingWritableSocket(ParkingWritableSocket const&) = delete;
    ParkingWritableSocket(ParkingWritableSocket&&) = delete;
    ParkingWritableSocket& operator=(ParkingWritableSocket const&) = delete;
    ParkingWritableSocket& operator=(ParkingWritableSocket&&) = delete;

    /// Abandons a write a case left parked, for @c ParkingReadableSocket's reason.
    ~ParkingWritableSocket() override
    {
        if (auto* const parked = std::exchange(_parked, nullptr); parked != nullptr)
            parked->abandon();
    }

    /// From now on every write parks, as against a peer whose receive window has closed.
    void stopReading() noexcept { _stopped = true; }

    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return _stopped ? park() : SocketDecorator::write(buffer);
    }

    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override
    {
        return _stopped ? park() : SocketDecorator::writeVectored(segments, std::move(keepAlive));
    }

    /// Retires a parked write, completing it LAST.
    void close() noexcept override
    {
        auto* const parked = std::exchange(_parked, nullptr);
        SocketDecorator::close();
        if (parked == nullptr)
            return;
        ++_writesRetiredByClose;
        completeCancelled(*parked);
    }

    /// @return Whether a write is parked right now.
    [[nodiscard]] bool isWriteParked() const noexcept { return _parked != nullptr; }

    /// @return How many parked writes @c close retrieved.
    [[nodiscard]] std::size_t writesRetiredByClose() const noexcept { return _writesRetiredByClose; }

  private:
    /// @return An awaitable recorded at its final address, which only @c close completes.
    [[nodiscard]] IoAwaitable park() noexcept
    {
        // The write slot's tripwire, as on every real socket: a second write over a parked one
        // would drop it.
        contract::claimWriteSlot(_parked);
        return IoAwaitable { [](void* owner, IoAwaitable& self) {
                                auto* const socket = static_cast<ParkingWritableSocket*>(owner);
                                contract::claimWriteSlot(socket->_parked);
                                socket->_parked = &self;
                            },
                             &ParkingWritableSocket::retire,
                             this };
    }

    /// Forgets a write whose flow is unwinding.
    static void retire(void* owner, void* awaitable) noexcept
    {
        auto* const socket = static_cast<ParkingWritableSocket*>(owner);
        if (socket->_parked == awaitable)
            socket->_parked = nullptr;
    }

    IoAwaitable* _parked { nullptr };
    bool _stopped { false };
    std::size_t _writesRetiredByClose { 0 };
};

} // namespace core::net::testing
