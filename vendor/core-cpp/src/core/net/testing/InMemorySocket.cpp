// SPDX-License-Identifier: Apache-2.0
#include <core/net/testing/InMemorySocket.hpp>

#include <core/Ranges.hpp>
#include <core/net/SocketContract.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace core::net::testing
{

// -- InMemoryPipe ---------------------------------------------------------------------------------

InMemoryPipe::InMemoryPipe(std::size_t maxBytesInFlight) noexcept: _maxInFlight { maxBytesInFlight }
{
}

std::size_t InMemoryPipe::push(std::span<std::byte const> bytes)
{
    if (_writeClosed || _reset)
        return 0;

    auto accepted = bytes.size();
    if (_maxInFlight != 0)
        accepted = std::min(accepted, _maxInFlight > _buffer.size() ? _maxInFlight - _buffer.size() : 0);

    std::ranges::copy(bytes.first(accepted), std::back_inserter(_buffer));
    return accepted;
}

std::size_t InMemoryPipe::pull(std::span<std::byte> into)
{
    auto const take = std::min(into.size(), _buffer.size());
    auto const end = _buffer.begin() + static_cast<std::ptrdiff_t>(take);
    std::ranges::copy(_buffer.begin(), end, into.begin());
    _buffer.erase(_buffer.begin(), end);
    return take;
}

void InMemoryPipe::closeWrite() noexcept
{
    _writeClosed = true;
    wakeReader();
}

bool InMemoryPipe::closeRead() noexcept
{
    _readClosed = true;
    return !_buffer.empty();
}

void InMemoryPipe::reset(NetErrorCode code) noexcept
{
    _reset = true;
    _resetCode = code;
    _buffer.clear();
    // Each wake-up is read from the member AT ITS CALL, never both up front: the first resumes a
    // coroutine that may destroy the socket the second one names, and a destroyed socket has
    // already deregistered itself here. The pipe itself outlives both calls, because every caller
    // holds a reference to it for the duration (see InMemorySocket's use of local copies).
    wakeReader();
    wakeWriter();
}

void InMemoryPipe::onReadable(Wake wake, void* state) noexcept
{
    _readable = wake;
    _readableState = state;
}

void InMemoryPipe::onWritable(Wake wake, void* state) noexcept
{
    _writable = wake;
    _writableState = state;
}

void InMemoryPipe::wakeReader() const noexcept
{
    if (_readable != nullptr)
        _readable(_readableState);
}

void InMemoryPipe::wakeWriter() const noexcept
{
    if (_writable != nullptr)
        _writable(_writableState);
}

// -- InMemorySocket -------------------------------------------------------------------------------

namespace
{
    /// The peer closed over bytes it had not read, so its stack sent the reset at once:
    /// `ECONNRESET` and `WSAECONNRESET`, which this library calls `ConnReset`.
    constexpr auto ResetByPeersClose = NetErrorCode::ConnReset;

    /// This end wrote after the peer's FIN, and that write drew the reset: `EPIPE` and
    /// `WSAECONNABORTED`, which this library calls `SystemError`. Measured on loopback
    /// ([fastcached#1553](https://github.com/LASTRADA-Software/fastcached/issues/1553)).
    constexpr auto AbortedByOwnWrite = NetErrorCode::SystemError;

    /// @param code How the reset arrived, as the pipe recorded it.
    /// @return What a reset connection answers, to reads and writes alike.
    [[nodiscard]] NetError resetError(NetErrorCode code)
    {
        return makeNetError(code, 0, "InMemorySocket: the connection was reset");
    }

    /// @param what The operation.
    /// @return What every operation on a closed end answers — the platform sockets' own check.
    [[nodiscard]] NetError closedSocket(char const* what)
    {
        return makeNetError(NetErrorCode::BadHandle, 0, std::string { what } + " on closed socket");
    }

    /// @param what Who retired the operation.
    /// @return What a parked operation retired by the RESOURCE answers — a value, not a throw.
    [[nodiscard]] NetError cancelledBy(char const* what)
    {
        return makeNetError(NetErrorCode::Cancelled, 0, std::string { "InMemorySocket: " } + what);
    }
} // namespace

InMemorySocket::InMemorySocket(std::shared_ptr<InMemoryPipe> inbound,
                               std::shared_ptr<InMemoryPipe> outbound,
                               std::string peerAddress) noexcept:
    _inbound { std::move(inbound) },
    _outbound { std::move(outbound) },
    _peerAddress { std::move(peerAddress) }
{
    _inbound->onReadable(&InMemorySocket::onInboundProgress, this);
    _outbound->onWritable(&InMemorySocket::onOutboundProgress, this);
}

InMemorySocket::~InMemorySocket()
{
    close(CloseMode::Abandon);
}

void InMemorySocket::close() noexcept
{
    close(CloseMode::Resolve);
}

void InMemorySocket::close(CloseMode mode) noexcept
{
    if (_closed)
        return;
    _closed = true;

    // **Detach everything first**: the wake-ups, so this end's own parked operations are answered
    // by the settle at the end and never by the reset it sends the peer, and both operations, into
    // locals. From here on nothing touches a member: every pipe call below wakes the PEER inline,
    // and a peer that owns this socket may destroy it before the call returns. The locals keep both
    // pipes alive through that.
    auto const inbound = _inbound;
    auto const outbound = _outbound;
    inbound->onReadable(nullptr, nullptr);
    outbound->onWritable(nullptr, nullptr);
    auto read = std::exchange(_read, {});
    auto write = std::exchange(_write, {});
    _readPending = {};
    _writePending = {};

    // A close with the peer's bytes still unread is answered with a reset rather than a FIN, by
    // every stack measured: the peer's writes fail from the next one on, and its reads report the
    // reset. Delivered before the FIN, so a peer parked on a read wakes to the reset and not to an
    // EOF that a real socket would never report.
    if (inbound->closeRead())
    {
        inbound->reset(ResetByPeersClose);
        outbound->reset(ResetByPeersClose);
    }
    outbound->closeWrite();

    // **Settled LAST**, both of them, and neither reads `this`: completing resumes a coroutine that
    // may own this socket and destroy it before the completion returns.
    auto const settle = [mode](IoAwaitable* awaitable) {
        if (awaitable == nullptr)
            return;
        if (mode == CloseMode::Abandon)
            awaitable->abandon();
        else
            awaitable->complete(std::unexpected(cancelledBy("the socket was closed")));
    };
    settle(read.awaitable);
    settle(write.awaitable);
}

void InMemorySocket::cancelRead() noexcept
{
    // Detached FIRST and completed LAST, as `close` does and for its reason. Unlike `close` this
    // leaves the socket usable: the wake-up stays installed and a later read works.
    auto* const parked = std::exchange(_read, {}).awaitable;
    if (parked != nullptr)
        parked->complete(std::unexpected(cancelledBy("cancelRead")));
}

ResultAwaitable<void> InMemorySocket::shutdownWrite()
{
    // After `close` there is no write side left to shut, and each platform socket answers success
    // without acting; so does this one.
    if (_closed)
        return ResultAwaitable<void> { std::expected<void, NetError> {} };

    auto const outbound = _outbound;
    outbound->closeWrite();
    return ResultAwaitable<void> { std::expected<void, NetError> {} };
}

std::optional<IoResult> InMemorySocket::tryRead(ReadKind kind, std::span<std::byte> buffer)
{
    auto const inbound = _inbound;

    // A reset is an error to every read verb, and never a readable byte
    // ([fastcached#899](https://github.com/LASTRADA-Software/fastcached/issues/899)).
    if (inbound->isReset())
        return std::unexpected(resetError(inbound->resetCode()));

    if (inbound->buffered() > 0)
    {
        if (kind == ReadKind::Probe)
            return IoResult { inbound->buffered() };
        return IoResult { inbound->pull(buffer) };
    }

    // EOF is "drained AND the writer finished", the same pair for both verbs. The latch is the
    // read's alone, as on the platform sockets: a probe reports EOF without consuming it.
    if (inbound->isWriteClosed())
    {
        if (kind == ReadKind::Bytes)
            _peerClosed = true;
        return IoResult { std::size_t { 0 } };
    }
    return std::nullopt;
}

IoAwaitable InMemorySocket::read(std::span<std::byte> buffer)
{
    contract::requireReadBuffer(buffer);
    if (_closed)
        return IoAwaitable { std::unexpected(closedSocket("read")) };

    if (auto const done = tryRead(ReadKind::Bytes, buffer); done.has_value())
    {
        // The pull made room; the writer is woken LAST, because it may own and destroy this socket.
        auto const inbound = _inbound;
        inbound->wakeWriter();
        return IoAwaitable { *done };
    }

    contract::claimReadSlot(_read.awaitable);
    _readPending = ReadOperation { .awaitable = nullptr, .kind = ReadKind::Bytes, .buffer = buffer };
    return IoAwaitable {
        [](void* owner, IoAwaitable& self) {
            auto* const socket = static_cast<InMemorySocket*>(owner);
            // The claim is HERE as well as in the verb, for `PosixSocket::read`'s reason: two
            // operations created and awaited afterwards pass the verb's check together.
            contract::claimReadSlot(socket->_read.awaitable);
            auto const pending = std::exchange(socket->_readPending, {});
            // Asked again, because this fake has no level-triggered readiness to catch what arrived
            // between the verb and the await: a push in that window woke nobody.
            if (auto const done = socket->tryRead(pending.kind, pending.buffer); done.has_value())
            {
                auto const inbound = socket->_inbound;
                self.complete(*done);
                inbound->wakeWriter();
                return;
            }
            socket->_read =
                ReadOperation { .awaitable = &self, .kind = pending.kind, .buffer = pending.buffer };
        },
        &InMemorySocket::retireRead,
        this
    };
}

IoAwaitable InMemorySocket::waitReadable()
{
    if (_closed)
        return IoAwaitable { std::unexpected(closedSocket("waitReadable")) };

    if (auto const done = tryRead(ReadKind::Probe, {}); done.has_value())
        return IoAwaitable { *done };

    contract::claimReadSlot(_read.awaitable);
    _readPending = ReadOperation { .awaitable = nullptr, .kind = ReadKind::Probe, .buffer = {} };
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<InMemorySocket*>(owner);
                            contract::claimReadSlot(socket->_read.awaitable);
                            auto const pending = std::exchange(socket->_readPending, {});
                            if (auto const done = socket->tryRead(pending.kind, pending.buffer);
                                done.has_value())
                            {
                                self.complete(*done);
                                return;
                            }
                            socket->_read =
                                ReadOperation { .awaitable = &self, .kind = pending.kind, .buffer = {} };
                        },
                         &InMemorySocket::retireRead,
                         this };
}

std::optional<IoResult> InMemorySocket::answerIfCannotWrite(std::size_t length) noexcept
{
    auto const outbound = _outbound;
    auto const inbound = _inbound;

    // This end's own half-close is `EPIPE` on POSIX and `WSAESHUTDOWN` on Windows, both
    // `SystemError`. Upstream once answered it as backpressure -- `WouldBlock`, the one failure a
    // caller is entitled to retry.
    if (outbound->isWriteClosed())
        return std::unexpected(makeNetError(
            NetErrorCode::SystemError, 0, "InMemorySocket: a write after this end's shutdownWrite"));
    if (outbound->isReset())
        return std::unexpected(resetError(outbound->resetCode()));
    if (!outbound->isReadClosed() || length == 0)
        return std::nullopt;

    // The first write after a graceful close: the bytes reach a peer that has gone and are lost,
    // and its stack answers with the reset that fails everything after. On a real socket that
    // answer arrives a round trip later -- on loopback, before the next call, measured on Linux,
    // macOS and Windows even with the writes back to back. The resets wake this end's own parked
    // read, which may destroy this socket, so nothing below touches a member.
    outbound->reset(AbortedByOwnWrite);
    inbound->reset(AbortedByOwnWrite);
    return IoResult { length };
}

std::optional<IoResult> InMemorySocket::trySend(WriteOperation& operation)
{
    auto const outbound = _outbound;
    while (operation.next < operation.unsent.size())
    {
        auto& front = operation.unsent[operation.next];
        auto const accepted = outbound->push(front);
        operation.written += accepted;
        front = front.subspan(accepted);
        if (!front.empty())
            return std::nullopt; // the bound is reached; the rest waits for the reader
        ++operation.next;
    }
    return IoResult { operation.written };
}

IoAwaitable InMemorySocket::write(std::span<std::byte const> buffer)
{
    auto const segments = std::array { buffer };
    return startWrite(segments, {});
}

IoAwaitable InMemorySocket::writeVectored(std::span<std::span<std::byte const> const> segments,
                                          std::shared_ptr<void const> keepAlive)
{
    return startWrite(segments, std::move(keepAlive));
}

IoAwaitable InMemorySocket::startWrite(std::span<std::span<std::byte const> const> segments,
                                       std::shared_ptr<void const> keepAlive)
{
    // Guard first, as `PosixSocket::write` does: a write that pushed its bytes past a parked one
    // would put them on the wire out of order, whatever it returned.
    contract::claimWriteSlot(_write.awaitable);

    if (_closed)
        return IoAwaitable { std::unexpected(closedSocket("write")) };

    // One write on the wire, so one answer when the peer has gone: a vectored write is the first
    // after a close, or a later one, exactly as a contiguous one would be.
    auto const length = core::ranges::FoldLeft(
        segments | std::views::transform([](auto const& segment) { return segment.size(); }),
        std::size_t { 0 },
        std::plus {});
    if (auto answer = answerIfCannotWrite(length); answer.has_value())
        return IoAwaitable { std::move(*answer) };

    auto operation = WriteOperation { .awaitable = nullptr,
                                      .unsent = { segments.begin(), segments.end() },
                                      .next = 0,
                                      .written = 0,
                                      .keepAlive = std::move(keepAlive) };
    auto const outbound = _outbound;
    if (auto const done = trySend(operation); done.has_value())
    {
        outbound->wakeReader(); // LAST: the reader may own and destroy this socket
        return IoAwaitable { *done };
    }

    // Parked with part of it sent: the reader is woken for what did go, after the operation is
    // recorded, so a reader that destroys this socket leaves nothing half-written into it.
    _writePending = std::move(operation);
    outbound->wakeReader();
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<InMemorySocket*>(owner);
                            contract::claimWriteSlot(socket->_write.awaitable);
                            auto pending = std::exchange(socket->_writePending, {});
                            pending.awaitable = &self;
                            socket->_write = std::move(pending);
                            // No retry here, unlike the read side: the bytes that fit were pushed by the
                            // verb, and room can only have appeared through a pull, which woke
                            // `onOutboundProgress` -- but with nothing parked yet it found nothing to pump.
                            // So pump once now.
                            onOutboundProgress(socket);
                        },
                         &InMemorySocket::retireWrite,
                         this };
}

void InMemorySocket::onInboundProgress(void* state) noexcept
{
    auto* const self = static_cast<InMemorySocket*>(state);
    if (self->_read.awaitable == nullptr)
        return;

    auto const inbound = self->_inbound;
    auto const kind = self->_read.kind;
    auto const buffer = self->_read.buffer;
    auto const answer = self->tryRead(kind, buffer);
    if (!answer.has_value())
        return;

    // Detached before the writer is woken or the reader completed: either resumes a coroutine that
    // may destroy this socket.
    auto* const awaitable = std::exchange(self->_read, {}).awaitable;
    if (kind == ReadKind::Bytes)
        inbound->wakeWriter();
    awaitable->complete(*answer);
}

void InMemorySocket::onOutboundProgress(void* state) noexcept
{
    auto* const self = static_cast<InMemorySocket*>(state);
    if (self->_write.awaitable == nullptr)
        return;

    auto const outbound = self->_outbound;
    auto operation = std::exchange(self->_write, {});

    // The peer may have gone while this write waited: a peer that closed over bytes it never read
    // reset the connection, and this end's own half-close fails what it has not yet sent.
    auto answer = std::optional<IoResult> {};
    if (outbound->isWriteClosed())
        answer = std::unexpected(makeNetError(
            NetErrorCode::SystemError, 0, "InMemorySocket: a write after this end's shutdownWrite"));
    else if (outbound->isReset())
        answer = std::unexpected(resetError(outbound->resetCode()));
    else
        answer = self->trySend(operation);

    if (!answer.has_value())
    {
        self->_write = std::move(operation);
        outbound->wakeReader();
        return;
    }

    auto* const awaitable = operation.awaitable;
    if (answer->has_value())
        outbound->wakeReader();
    awaitable->complete(*answer);
}

void InMemorySocket::retireRead(void* owner, void* awaitable) noexcept
{
    auto* const socket = static_cast<InMemorySocket*>(owner);
    if (socket->_read.awaitable == awaitable)
        socket->_read = {};
}

void InMemorySocket::retireWrite(void* owner, void* awaitable) noexcept
{
    auto* const socket = static_cast<InMemorySocket*>(owner);
    if (socket->_write.awaitable == awaitable)
        socket->_write = {};
}

// -- InMemorySocketPair ---------------------------------------------------------------------------

InMemorySocketPair InMemorySocketPair::create(std::size_t maxBytesInFlight, std::string serverPeerAddress)
{
    auto clientToServer = std::make_shared<InMemoryPipe>(maxBytesInFlight);
    auto serverToClient = std::make_shared<InMemoryPipe>(maxBytesInFlight);
    return InMemorySocketPair {
        .client = std::make_unique<InMemorySocket>(serverToClient, clientToServer),
        .server =
            std::make_unique<InMemorySocket>(clientToServer, serverToClient, std::move(serverPeerAddress)),
    };
}

// -- InMemoryListener -----------------------------------------------------------------------------

InMemoryListener::~InMemoryListener()
{
    if (auto* const parked = std::exchange(_pending, nullptr); parked != nullptr)
        parked->abandon();
}

async::Task<AcceptResult> InMemoryListener::accept()
{
    // Connections queued before a close are still handed out, so a server finishes what it had
    // accepted before it observes the shutdown.
    if (!_ready.empty())
    {
        auto socket = std::move(_ready.front());
        _ready.pop_front();
        co_return AcceptResult { std::move(socket) };
    }
    if (_closed)
        co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "InMemoryListener: closed"));

    co_return co_await AcceptAwaitable {
        [](void* owner, AcceptAwaitable& self) {
            auto* const listener = static_cast<InMemoryListener*>(owner);
            // One accept at a time, the way a listener's accept loop has one: a second would drop
            // the first exactly as a second read drops a parked one -- never resumed, never freed.
            assert(listener->_pending == nullptr
                   && "an accept was armed over a parked one: the parked accept would never be resumed "
                      "and its frame never freed (see core/net/testing/InMemorySocket.hpp)");
            listener->_pending = &self;
            listener->completePendingAccept();
        },
        &InMemoryListener::retireAccept,
        this
    };
}

void InMemoryListener::close() noexcept
{
    _closed = true;
    if (auto* const parked = std::exchange(_pending, nullptr); parked != nullptr)
        parked->complete(
            std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "InMemoryListener: closed")));
}

std::unique_ptr<InMemorySocket> InMemoryListener::connectClient(std::size_t maxBytesInFlight,
                                                                std::string peerAddress)
{
    auto pair = InMemorySocketPair::create(maxBytesInFlight, std::move(peerAddress));
    _ready.push_back(std::move(pair.server));
    completePendingAccept();
    return std::move(pair.client);
}

void InMemoryListener::completePendingAccept() noexcept
{
    if (_pending == nullptr || _ready.empty())
        return;
    auto* const parked = std::exchange(_pending, nullptr);
    auto socket = std::move(_ready.front());
    _ready.pop_front();
    parked->complete(AcceptResult { std::move(socket) });
}

void InMemoryListener::retireAccept(void* owner, void* awaitable) noexcept
{
    auto* const listener = static_cast<InMemoryListener*>(owner);
    if (listener->_pending == awaitable)
        listener->_pending = nullptr;
}

} // namespace core::net::testing
