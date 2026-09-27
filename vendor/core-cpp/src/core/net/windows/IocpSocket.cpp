// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
// clang-format on

#include <core/net/windows/IocpSocket.hpp>

#include <core/net/SocketAddress.hpp>
#include <core/net/SocketContract.hpp>
#include <core/net/detail/PeerAddress.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/IocpOperation.hpp>
#include <core/net/windows/UnixSocketPath.hpp>
#include <core/net/windows/WinsockError.hpp>
#include <core/platform/WinsockInit.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstring>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace core::net
{

namespace
{

    /// The most one `WSABUF` may say it carries. A larger span is sent in pieces, which a partial
    /// completion already has to handle.
    constexpr auto MaxBufferLength = std::size_t { std::numeric_limits<ULONG>::max() };

    /// The most one overlapped receive takes into its node's own buffer. A read may return fewer
    /// bytes than its span holds, so this bounds the copy and the allocation, not the read.
    constexpr auto MaxOwnedReceive = std::size_t { 64 } * 1024;

    /// The most one overlapped send copies into its node's own buffer. A partial send already
    /// re-issues the rest, so a larger write simply takes more operations.
    constexpr auto MaxOwnedSend = std::size_t { 256 } * 1024;

    /// How many segments one gathered `WSASend` carries. The cursor drives the rest, so this
    /// bounds a vector's growth rather than the write.
    constexpr auto MaxSegmentsPerSend = std::size_t { 64 };

    /// `AcceptEx` wants room for each address plus 16 bytes; `sockaddr_storage` so an IPv6 peer is
    /// never truncated (fastcached `Net/IocpSocket.cpp`).
    constexpr auto AcceptAddressSize = DWORD { sizeof(sockaddr_storage) + 16 };

    /// @param what Which verb is refusing.
    /// @return The error a verb reports on a socket that is already closed.
    [[nodiscard]] NetError closedSocket(char const* what)
    {
        return makeNetError(NetErrorCode::BadHandle, 0, std::string { what } + " on closed socket");
    }

    /// @param what Why.
    /// @return A resource-side cancellation, which a flow receives as a VALUE.
    [[nodiscard]] NetError cancelled(char const* what)
    {
        return makeNetError(NetErrorCode::Cancelled, 0, what);
    }

    /// @return What a verb reports when the loop would not park the operation. Distinct from a
    ///         cancellation: nothing was cancelled, the park was refused.
    [[nodiscard]] NetError parkRefused()
    {
        return makeNetError(NetErrorCode::SystemError, 0, "the event loop refused to park this operation");
    }

    /// Fetches a Winsock extension function (`AcceptEx`, `ConnectEx`, ...) for @p socket.
    /// @tparam Function The function-pointer type.
    /// @param socket A socket of the provider whose extension is wanted.
    /// @param id The extension's GUID.
    /// @return The function, or null.
    template <typename Function>
    [[nodiscard]] Function extensionFunction(SOCKET socket, GUID id) noexcept
    {
        auto function = Function { nullptr };
        auto returned = DWORD { 0 };
        if (::WSAIoctl(socket,
                       SIO_GET_EXTENSION_FUNCTION_POINTER,
                       &id,
                       sizeof(id),
                       // Explicit: `&function` is a pointer to a FUNCTION pointer, and the analyser
                       // refuses letting that reach `LPVOID` implicitly.
                       static_cast<void*>(&function),
                       sizeof(function),
                       &returned,
                       nullptr,
                       nullptr)
            != 0)
            return nullptr;
        return function;
    }
} // namespace

// ---- The operation node -----------------------------------------------------------------------

/// One overlapped operation of an @c IocpSocket: the `OVERLAPPED` the kernel is handed, and what
/// the socket needs to know when it comes back.
///
/// **It holds itself while the kernel holds it** (@c self), and the port gives that share back
/// when the packet is dequeued — so the kernel's pointer is never into the socket, and a socket
/// destroyed mid-operation leaves the kernel writing into a node that still exists
/// ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)). The socket holds
/// a second share while the node is in one of its slots.
///
/// **One node per ARMED operation, never re-armed while the kernel holds it.** A read retired by
/// `cancelRead` keeps its node until its completion is dequeued; the next read gets a fresh one.
/// Reusing it is how upstream turned an abort into a spurious EOF on a healthy socket: the second
/// `WSARecv` cleared the `OVERLAPPED` the kernel had just written `STATUS_CANCELLED` into, and the
/// abort dispatched as a successful read of zero bytes
/// ([fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)). A write re-uses
/// its node only once its own completion has come back, to send what a partial completion left.
///
/// **The kernel is never handed the CALLER's memory, only the node's own** (@c owned). A caller's
/// buffer is borrowed for as long as its operation is awaited, and every abandon path ends that
/// borrow before the kernel is done: an awaitable destroyed while parked, a socket destroyed under
/// one, a loop torn down. `CancelIoEx` only ASKS for the operation back, so a receive the peer's
/// data reaches first still completes -- and it would complete into memory the caller has freed.
/// So an overlapped receive lands in @c owned and is copied out when its completion is delivered to
/// a flow that is still there; an overlapped send is copied into @c owned when it is issued. The
/// copy is paid only by an operation that had to wait: a read of data already there and a write
/// the send buffer takes go straight to and from the caller's memory, synchronously.
struct IocpSocket::Node: detail::IocpOperation
{
    /// Which verb armed this node.
    enum class Kind : std::uint8_t
    {
        Bytes, ///< @c read: receive into the caller's buffer.
        Probe, ///< @c waitReadable: a zero-byte receive, then a `MSG_PEEK`.
        Write, ///< @c write or @c writeVectored.
    };

    Kind kind = Kind::Bytes;
    IocpSocket* socket = nullptr;     ///< The owner; valid while a park or a timer names this node.
    IoAwaitable* awaitable = nullptr; ///< The parked awaitable; null until armed. Borrowed.
    ParkId park {};                   ///< The loop park the completion wakes.
    TimerId deadline {};              ///< A read's receive deadline, while it is armed.
    bool deadlineFired = false;       ///< The deadline asked the kernel for this read back.
    bool stopped = false;             ///< A stop of the awaiting flow asked for it back.

    /// The kernel's share: set when the operation is issued, given back by @c released.
    std::shared_ptr<Node> self;

    std::span<std::byte> buffer; ///< A read's destination.

    std::span<std::byte const> flat;                      ///< A flat write's source.
    std::span<std::span<std::byte const> const> segments; ///< A gathered write's segments.
    std::size_t segmentIndex = 0;                         ///< Segments fully sent.
    std::size_t segmentOffset = 0;                        ///< How far into @c segmentIndex's segment is sent.
    std::size_t written = 0;                              ///< The running total a write resolves with.

    /// Pins a gathered write's payload. Released with the node, which is no earlier than the
    /// kernel's last read of it — the second defect of #465, which on a destroyed socket was
    /// corruption on the wire rather than a crash.
    std::shared_ptr<void const> keepAlive;

    /// What the kernel reads from or writes into while it holds this node, in place of the caller's
    /// buffer; see the type's comment. Lives exactly as long as the node, which the kernel's share
    /// keeps alive until the port dequeues the packet.
    std::vector<std::byte> owned;

    /// The `WSABUF`s a send was issued with. Winsock captures the array at the call, but it is
    /// kept here anyway: it costs nothing and removes a question.
    std::vector<WSABUF> buffers;

    /// @return What this write still owes.
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        if (segments.empty())
            return flat.size() - written;
        auto left = std::size_t { 0 };
        for (auto const index: std::views::iota(segmentIndex, segments.size()))
            left += segments[index].size() - (index == segmentIndex ? segmentOffset : 0);
        return left;
    }

    /// The port's dequeue hook: gives back the kernel's share, which may free this node.
    /// @param operation This node.
    static void released(detail::IocpOperation& operation) noexcept
    {
        auto const keep = std::move(static_cast<Node&>(operation).self);
    }
};

// ---- IocpSocket -------------------------------------------------------------------------------

IocpSocket::IocpSocket(EventLoop& loop,
                       SOCKET socket,
                       std::string peerAddress,
                       IocpAssociation association) noexcept:
    _loop(loop), _port(loop.completionPort()), _socket(socket), _peerAddress(std::move(peerAddress))
{
    assert(_port != nullptr
           && "an IocpSocket was built on a loop whose backend lends no completion port; nothing "
              "would ever complete its operations (use the socket factories, which ask)");
    if (_port == nullptr)
        _associationError =
            makeNetError(NetErrorCode::Unsupported, 0, "the loop's backend lends no completion port");
    else if (_socket == detail::InvalidSocket)
        _associationError = makeNetError(NetErrorCode::BadHandle, 0, "IocpSocket: invalid socket");
    else if (association == IocpAssociation::Associate)
    {
        if (auto associated = _port->associate(reinterpret_cast<platform::NativeHandle>(_socket));
            !associated)
            _associationError = std::move(associated.error());
    }

    // Non-blocking, for the ONE non-overlapped call this socket makes: the `MSG_PEEK` that measures
    // what a completed readability probe found. An overlapped operation ignores the mode, so this
    // changes nothing else -- libuv runs its Windows sockets the same way.
    if (_socket != detail::InvalidSocket)
    {
        auto nonBlocking = u_long { 1 };
        // FIONBIO is an unsigned constant and `ioctlsocket` takes a signed command, as
        // `platform/SystemPipe.cpp` says where it does the same.
        std::ignore = ::ioctlsocket(_socket, static_cast<long>(FIONBIO), &nonBlocking);
    }
}

IocpSocket::~IocpSocket()
{
    contract::assertTeardownIsSerialisedWithDispatch(_loop);
    // Cancel, not Resume: a flow resumed on its normal path would look at a socket that is about
    // to stop existing. The operations themselves outlive it on their own shares.
    close(FdWakePolicy::Cancel);
}

void IocpSocket::close() noexcept
{
    close(FdWakePolicy::Resume);
}

void IocpSocket::close(FdWakePolicy policy) noexcept
{
    if (_closed)
        return;
    _closed = true;

    // **Detached FIRST, completed LAST, with no member touched in between.** Completing resumes a
    // flow that may own this socket and destroy it before `complete` returns, so every operation
    // is taken into locals before any of them is settled. The nodes the locals hold keep what the
    // kernel still writes into alive on their own; closing the socket is what makes those
    // operations complete, and their packets arrive later to nobody.
    auto nodes = std::vector<std::shared_ptr<Node>> {};
    nodes.reserve(_settling.size() + 2);
    if (_read)
        nodes.push_back(_read);
    if (_write)
        nodes.push_back(_write);
    std::ranges::copy(_settling, std::back_inserter(nodes));

    auto parked = std::vector<IoAwaitable*> {};
    parked.reserve(nodes.size());
    for (auto const& node: nodes)
        if (auto* const awaitable = take(*node); awaitable != nullptr)
            parked.push_back(awaitable);

    if (_socket != detail::InvalidSocket)
    {
        // Forgotten BEFORE the close: an association ends with the handle, Windows reuses handle
        // values, and a record left standing would make the next socket handed this value look
        // associated and complete nowhere (`ICompletionPort::forget`).
        if (_port != nullptr && !_associationError.has_value())
            _port->forget(reinterpret_cast<platform::NativeHandle>(_socket));
        ::closesocket(_socket);
        _socket = detail::InvalidSocket;
    }

    // Past this point nothing may touch a member.
    for (auto* const awaitable: parked)
    {
        if (policy == FdWakePolicy::Cancel)
            awaitable->abandon();
        else
            awaitable->complete(std::unexpected(cancelled("the socket was closed")));
    }
}

std::optional<NetError> IocpSocket::unusable(char const* what) const
{
    if (_closed || _socket == detail::InvalidSocket)
        return closedSocket(what);
    return _associationError;
}

std::expected<void, NetError> IocpSocket::park(Node& node)
{
    auto refusal = NetError {};
    node.park = detail::parkOnCompletion(_loop,
                                         node,
                                         node.kind == Node::Kind::Write ? &IocpSocket::onWriteWake
                                                                        : &IocpSocket::onReadWake,
                                         &node,
                                         &refusal);
    if (node.park)
        return {};
    return std::unexpected(refusal.code == NetErrorCode::Ok ? parkRefused() : std::move(refusal));
}

template <typename Issue>
std::expected<void, NetError> IocpSocket::handToKernel(std::shared_ptr<Node> const& node,
                                                       Issue issue,
                                                       char const* what)
{
    auto* const operation = static_cast<detail::IocpOperation*>(node.get());
    node->rearm();
    node->onDequeued = &Node::released;
    // Announced BEFORE the call: the packet can be queued the instant it is issued.
    _port->beginOperation(operation);
    node->self = node;
    auto const error = issue(reinterpret_cast<LPWSAOVERLAPPED>(&node->overlapped));
    // 0 means it completed at once, and the packet is STILL queued -- nothing here asks
    // `SetFileCompletionNotificationModes` to skip it -- so both answers mean the same thing.
    if (error == 0 || error == WSA_IO_PENDING)
        return {};
    _port->withdrawOperation(operation);
    node->self.reset();
    return std::unexpected(detail::fromWinsockError(error, what));
}

void IocpSocket::cancelInKernel(Node& node) const noexcept
{
    // Only while the kernel holds it: `self` is set from the issue to the dequeue. `CancelIoEx`
    // does not take the operation back, it ASKS for it back -- the completion still arrives,
    // carrying the abort or whatever the operation had already done.
    if (node.self && !node.completed && _socket != detail::InvalidSocket)
        std::ignore = ::CancelIoEx(reinterpret_cast<HANDLE>(_socket), &node.overlapped);
}

IoAwaitable* IocpSocket::take(Node& node) noexcept
{
    auto* const awaitable = std::exchange(node.awaitable, nullptr);
    if (node.deadline)
        std::ignore = _loop.cancelTimer(std::exchange(node.deadline, TimerId {}));
    if (node.park)
        _loop.unregisterPark(std::exchange(node.park, ParkId {}));

    // The socket's share goes LAST, because it may be the last one: nothing reads `node` after.
    if (_read.get() == &node)
        _read.reset();
    else if (_write.get() == &node)
        _write.reset();
    else
        std::erase_if(_settling, [&node](auto const& held) { return held.get() == &node; });
    return awaitable;
}

// ---- The read side ----------------------------------------------------------------------------

IoAwaitable IocpSocket::read(std::span<std::byte> buffer)
{
    contract::requireReadBuffer(buffer);
    if (auto error = unusable("read"))
        return IoAwaitable { std::unexpected(std::move(*error)) };

    // The verb's check is the early, friendlier diagnostic; the arm below checks again, because it
    // is where the slot is actually taken.
    auto* parked = _read ? _read->awaitable : nullptr;
    contract::claimReadSlot(parked, _socket);

    // Bytes already here are taken now, with no operation and no turn -- which is what every
    // other socket in this library does, and what keeps a caller's turn count the same on every
    // platform. Only a read that would block becomes an overlapped one.
    if (auto done = tryReceive(buffer); done.has_value())
        return IoAwaitable { std::move(*done) };

    _read = std::make_shared<Node>();
    _read->kind = Node::Kind::Bytes;
    _read->socket = this;
    _read->buffer = buffer;
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<IocpSocket*>(owner);
                            auto const node = socket->_read;
                            auto* claimed = node ? node->awaitable : nullptr;
                            contract::claimReadSlot(claimed, socket->_socket);
                            if (!node)
                            {
                                self.complete(
                                    std::unexpected(cancelled("the read was retired before it was awaited")));
                                return;
                            }
                            node->awaitable = &self;
                            if (auto issued = socket->issueRead(node); !issued)
                            {
                                std::ignore = socket->take(*node);
                                self.complete(std::unexpected(std::move(issued.error())));
                                return;
                            }
                            self.cancelThrough(socket->_loop, node->park);
                        },
                         &IocpSocket::retireRead,
                         this };
}

IoAwaitable IocpSocket::waitReadable()
{
    if (auto error = unusable("waitReadable"))
        return IoAwaitable { std::unexpected(std::move(*error)) };

    auto* parked = _read ? _read->awaitable : nullptr;
    contract::claimReadSlot(parked, _socket);

    if (auto done = tryPeek(); done.has_value())
        return IoAwaitable { std::move(*done) };

    _read = std::make_shared<Node>();
    _read->kind = Node::Kind::Probe;
    _read->socket = this;
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<IocpSocket*>(owner);
                            auto const node = socket->_read;
                            auto* claimed = node ? node->awaitable : nullptr;
                            contract::claimReadSlot(claimed, socket->_socket);
                            if (!node)
                            {
                                self.complete(std::unexpected(
                                    cancelled("the probe was retired before it was awaited")));
                                return;
                            }
                            node->awaitable = &self;
                            if (auto issued = socket->issueRead(node); !issued)
                            {
                                std::ignore = socket->take(*node);
                                self.complete(std::unexpected(std::move(issued.error())));
                                return;
                            }
                            self.cancelThrough(socket->_loop, node->park);
                        },
                         &IocpSocket::retireRead,
                         this };
}

std::expected<void, NetError> IocpSocket::issueRead(std::shared_ptr<Node> const& node)
{
    if (!node->park)
    {
        if (auto parked = park(*node); !parked)
            return parked;
    }

    // Into the node's own buffer, never the caller's: see `Node`. A probe receives nothing at all.
    if (node->kind == Node::Kind::Bytes)
        node->owned.resize(std::min(node->buffer.size(), MaxOwnedReceive));
    auto const destination =
        node->kind == Node::Kind::Probe ? std::span<std::byte> {} : std::span { node->owned };
    auto issued = handToKernel(
        node,
        [this, destination](LPWSAOVERLAPPED overlapped) -> int {
            // Zero bytes for a probe: the documented Winsock idiom for "complete when data is
            // pending, consuming nothing" (fastcached `Net/IocpSocket.cpp:555-561`).
            auto buffer = WSABUF { .len = static_cast<ULONG>(std::min(destination.size(), MaxBufferLength)),
                                   .buf = reinterpret_cast<CHAR*>(destination.data()) };
            auto flags = DWORD { 0 };
            return ::WSARecv(_socket, &buffer, 1, nullptr, &flags, overlapped, nullptr) == 0
                       ? 0
                       : ::WSAGetLastError();
        },
        node->kind == Node::Kind::Probe ? "WSARecv(0)" : "WSARecv");
    if (!issued)
        return issued;

    // Armed HERE and nowhere else, so it bounds exactly one read that had to wait -- and it is a
    // timer on the loop's ONE deadline heap. On expiry it asks the kernel for the receive back and
    // lets the completion answer, so bytes that beat the deadline are still delivered.
    if (_receiveDeadline > std::chrono::milliseconds::zero() && !node->deadline)
        node->deadline =
            _loop.addTimer(_loop.clock().now() + _receiveDeadline, &IocpSocket::onReadDeadline, node.get());
    return {};
}

IoResult IocpSocket::readResult(Node& node)
{
    auto const error = detail::completionError(_socket, node);
    if (error != 0)
    {
        if (detail::isAbort(error) && node.deadlineFired && !node.stopped)
            return std::unexpected(makeNetError(NetErrorCode::Timeout,
                                                static_cast<int>(error),
                                                "the receive deadline elapsed before any data"));
        return std::unexpected(detail::fromWinsockError(
            static_cast<int>(error), node.kind == Node::Kind::Probe ? "WSARecv(0)" : "WSARecv"));
    }

    if (node.kind == Node::Kind::Bytes)
    {
        auto const received = node.bytesTransferred();
        if (received == 0)
            _peerClosed = true; // isClosed() now answers true, as ISocket documents
        // Copied out HERE, where the flow that owns the destination is known to be waiting for it
        // -- this is only reached on the way to completing its awaitable.
        std::ranges::copy(std::span { node.owned }.first(received), node.buffer.begin());
        return IoResult { received };
    }

    // **A zero-byte receive completes with zero bytes whatever is waiting**, so the answer is
    // MEASURED rather than read off the completion: one `MSG_PEEK` of one byte, consuming nothing
    // (fastcached `Net/IocpSocket.cpp:327-334`, [fastcached#677]). `0` is EOF, `>0` is pending data.
    // A readiness with nothing behind it by the time it was looked at is reported as pending data
    // -- the fail-safe direction `ISocket::waitReadable` names -- so the caller's read parks and
    // learns the truth, rather than being told its peer is gone.
    return tryPeek().value_or(IoResult { std::size_t { 1 } });
}

std::optional<IoResult> IocpSocket::tryReceive(std::span<std::byte> buffer)
{
    auto const got = ::recv(_socket,
                            reinterpret_cast<char*>(buffer.data()),
                            static_cast<int>(std::min(buffer.size(), std::size_t { INT_MAX })),
                            0);
    if (got > 0)
        return IoResult { static_cast<std::size_t>(got) };
    if (got == 0)
    {
        _peerClosed = true; // isClosed() now answers true, as ISocket documents
        return IoResult { std::size_t { 0 } };
    }
    auto const err = ::WSAGetLastError();
    if (err == WSAEWOULDBLOCK)
        return std::nullopt;
    return IoResult { std::unexpected(detail::fromWinsockError(err, "recv")) };
}

std::optional<IoResult> IocpSocket::tryPeek() const
{
    // One byte, peeked: it consumes nothing the following read would have returned, and the count
    // it measures IS the contract -- `0` is EOF, `>0` is data pending.
    auto probe = std::array<char, 1> {};
    auto const got = ::recv(_socket, probe.data(), 1, MSG_PEEK);
    if (got >= 0)
        return IoResult { got == 0 ? std::size_t { 0 } : std::size_t { 1 } };
    auto const err = ::WSAGetLastError();
    if (err == WSAEWOULDBLOCK)
        return std::nullopt;
    // Anything else is the peer GONE, a reset above all, and "one byte is pending" would say the
    // opposite to a caller that only watches
    // ([fastcached#899](https://github.com/LASTRADA-Software/fastcached/issues/899)).
    return IoResult { std::unexpected(detail::fromWinsockError(err, "recv(MSG_PEEK)")) };
}

void IocpSocket::onReadWake(void* state, ParkWake wake)
{
    auto& node = *static_cast<Node*>(state);
    auto* const socket = node.socket;
    if (node.awaitable == nullptr)
        return;

    switch (wake)
    {
        case ParkWake::Ready: {
            if (!node.completed)
                return; // a report for an operation re-issued since; its own completion follows
            auto result = socket->readResult(node);
            auto* const awaitable = socket->take(node);
            awaitable->complete(std::move(result));
            return;
        }
        case ParkWake::Cancelled: {
            node.stopped = true;
            // The completion already arrived: the value wins over the stop
            // ([fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)).
            if (node.completed)
            {
                auto result = socket->readResult(node);
                auto* const awaitable = socket->take(node);
                awaitable->complete(std::move(result));
                return;
            }
            // A probe is a ZERO-byte receive: there is nothing in it to lose, so it is settled
            // now. `await_resume` turns the value into `OperationCancelled`, because the token it
            // reads is the one that was stopped.
            if (node.kind == Node::Kind::Probe)
            {
                socket->cancelInKernel(node);
                auto* const awaitable = socket->take(node);
                awaitable->complete(std::unexpected(cancelled("the awaiting flow was cancelled")));
                return;
            }
            // A real receive is still the kernel's, and may complete with bytes before the cancel
            // takes. The loop has detached the park the stop came through, so a fresh one is
            // registered to hear the completion on, and the flow resumes when it lands: bytes that
            // beat the cancel are handed over rather than thrown away.
            socket->_loop.unregisterPark(std::exchange(node.park, ParkId {}));
            socket->cancelInKernel(node);
            if (auto parked = socket->park(node); !parked)
            {
                auto* const awaitable = socket->take(node);
                awaitable->complete(std::unexpected(cancelled("the awaiting flow was cancelled")));
            }
            return;
        }
        case ParkWake::Abandoned: {
            // The loop is going away: nothing will dequeue the completion here, so the flow
            // unwinds now and the node outlives it on the kernel's share.
            socket->cancelInKernel(node);
            auto* const awaitable = socket->take(node);
            awaitable->abandon();
            return;
        }
    }
}

void IocpSocket::onReadDeadline(void* state)
{
    auto& node = *static_cast<Node*>(state);
    // Spent: the timer has fired, so `take` must not cancel it.
    node.deadline = TimerId {};
    if (node.awaitable == nullptr || node.completed)
        return; // the completion beat the deadline, and its bytes are on their way
    node.deadlineFired = true;
    node.socket->cancelInKernel(node);
}

void IocpSocket::retireRead(void* owner, void* awaitable) noexcept
{
    auto* const socket = static_cast<IocpSocket*>(owner);
    // Identity, not merely "something is parked": an operation retired and replaced must not be
    // able to retire its successor.
    auto node = std::shared_ptr<Node> {};
    if (socket->_read && socket->_read->awaitable == awaitable)
        node = socket->_read;
    else if (auto const found = std::ranges::find_if(
                 socket->_settling, [awaitable](auto const& held) { return held->awaitable == awaitable; });
             found != socket->_settling.end())
        node = *found;
    if (!node)
        return;
    // The flow is going away and its buffer with it, so the operation is asked back. It completes
    // later into its own node.
    socket->cancelInKernel(*node);
    std::ignore = socket->take(*node);
}

void IocpSocket::cancelRead() noexcept
{
    if (_closed || !_read || _read->awaitable == nullptr)
        return;

    auto node = _read;
    if (node->kind == Node::Kind::Probe && !node->completed)
    {
        // **A probe IS settled at once**: a zero-byte receive carries nothing, and completing it
        // here also keeps it away from the `MSG_PEEK` that would otherwise answer it on a later
        // turn and could report an EOF nobody observed. Its flow is resumed by the loop.
        cancelInKernel(*node);
        auto* const awaitable = take(*node);
        awaitable->complete(std::unexpected(cancelled("the read was retired by cancelRead")));
        return;
    }

    // **A real read SETTLES**: see the class comment. The slot is free when this returns, which is
    // the half that is synchronous -- the next read gets a node of its own and never the
    // `OVERLAPPED` the kernel still owns -- and the waiter resolves with whatever its own
    // operation did.
    cancelInKernel(*node);
    _settling.push_back(std::move(_read));
}

void IocpSocket::setReceiveDeadline(std::chrono::milliseconds deadline) noexcept
{
    // Non-positive REMOVES the bound, which is SO_RCVTIMEO's reading of zero; a read already
    // parked keeps the timer it armed.
    _receiveDeadline = std::max(deadline, std::chrono::milliseconds::zero());
}

ResultAwaitable<void> IocpSocket::shutdownWrite()
{
    // The precondition -- no write outstanding -- is the caller's, and is not asserted here, as it
    // is not on any other plain socket: `SocketContractCanary`'s write-slot-inline mode breaks it on
    // purpose to reach the write-slot guard behind it, and a second assertion in front would take
    // the guard's place.
    if (_closed || _socket == detail::InvalidSocket)
        return ResultAwaitable<void> { std::expected<void, NetError> {} };
    if (::shutdown(_socket, SD_SEND) == SOCKET_ERROR)
    {
        auto const err = ::WSAGetLastError();
        // WSAENOTCONN is the state the caller asked for, not a failure to report.
        if (err != WSAENOTCONN)
            return ResultAwaitable<void> { std::unexpected(detail::fromWinsockError(err, "shutdown")) };
    }
    return ResultAwaitable<void> { std::expected<void, NetError> {} };
}

// ---- The write side ---------------------------------------------------------------------------

IoAwaitable IocpSocket::write(std::span<std::byte const> buffer)
{
    auto* parked = _write ? _write->awaitable : nullptr;
    contract::claimWriteSlot(parked, _socket);
    if (auto error = unusable("write"))
        return IoAwaitable { std::unexpected(std::move(*error)) };
    if (buffer.empty())
        return IoAwaitable { IoResult { std::size_t { 0 } } };

    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::Write;
    node->socket = this;
    node->flat = buffer;
    // Sent now, as far as the send buffer takes it; only what would block goes overlapped.
    if (auto done = trySend(*node); done.has_value())
        return IoAwaitable { std::move(*done) };

    _write = std::move(node);
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<IocpSocket*>(owner);
                            auto const node = socket->_write;
                            auto* claimed = node ? node->awaitable : nullptr;
                            contract::claimWriteSlot(claimed, socket->_socket);
                            if (!node)
                            {
                                self.complete(std::unexpected(
                                    cancelled("the write was retired before it was awaited")));
                                return;
                            }
                            node->awaitable = &self;
                            if (auto issued = socket->issueWrite(node); !issued)
                            {
                                std::ignore = socket->take(*node);
                                self.complete(std::unexpected(std::move(issued.error())));
                                return;
                            }
                            self.cancelThrough(socket->_loop, node->park);
                        },
                         &IocpSocket::retireWrite,
                         this };
}

IoAwaitable IocpSocket::writeVectored(std::span<std::span<std::byte const> const> segments,
                                      std::shared_ptr<void const> keepAlive)
{
    auto* parked = _write ? _write->awaitable : nullptr;
    contract::claimWriteSlot(parked, _socket);
    if (auto error = unusable("write"))
        return IoAwaitable { std::unexpected(std::move(*error)) };

    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::Write;
    node->socket = this;
    node->segments = segments;
    node->keepAlive = std::move(keepAlive);
    if (node->remaining() == 0)
        return IoAwaitable { IoResult { std::size_t { 0 } } };
    if (auto done = trySend(*node); done.has_value())
        return IoAwaitable { std::move(*done) };

    _write = std::move(node);
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<IocpSocket*>(owner);
                            auto const node = socket->_write;
                            auto* claimed = node ? node->awaitable : nullptr;
                            contract::claimWriteSlot(claimed, socket->_socket);
                            if (!node)
                            {
                                self.complete(std::unexpected(
                                    cancelled("the write was retired before it was awaited")));
                                return;
                            }
                            node->awaitable = &self;
                            if (auto issued = socket->issueWrite(node); !issued)
                            {
                                std::ignore = socket->take(*node);
                                self.complete(std::unexpected(std::move(issued.error())));
                                return;
                            }
                            self.cancelThrough(socket->_loop, node->park);
                        },
                         &IocpSocket::retireWrite,
                         this };
}

std::shared_ptr<IocpSocket::Node> IocpSocket::holding(Node const& node) const noexcept
{
    if (_write.get() == &node)
        return _write;
    if (_read.get() == &node)
        return _read;
    auto const found =
        std::ranges::find_if(_settling, [&node](auto const& held) { return held.get() == &node; });
    return found != _settling.end() ? *found : nullptr;
}

std::expected<void, NetError> IocpSocket::issueWrite(std::shared_ptr<Node> const& node)
{
    if (!node->park)
    {
        if (auto parked = park(*node); !parked)
            return parked;
    }

    // A copy of what the cursor still owes, never the caller's bytes: see `Node`.
    copyOwed(*node);
    return handToKernel(
        node,
        [this, &node](LPWSAOVERLAPPED overlapped) -> int {
            return ::WSASend(_socket,
                             node->buffers.data(),
                             static_cast<DWORD>(node->buffers.size()),
                             nullptr,
                             0,
                             overlapped,
                             nullptr)
                           == 0
                       ? 0
                       : ::WSAGetLastError();
        },
        "WSASend");
}

void IocpSocket::copyOwed(Node& node)
{
    node.owned.clear();
    node.owned.reserve(std::min(node.remaining(), MaxOwnedSend));
    if (node.segments.empty())
    {
        auto const left = node.flat.subspan(node.written);
        auto const take = left.first(std::min(left.size(), MaxOwnedSend));
        node.owned.assign(take.begin(), take.end());
    }
    else
    {
        auto offset = node.segmentOffset;
        for (auto const index: std::views::iota(node.segmentIndex, node.segments.size()))
        {
            auto const segment = node.segments[index].subspan(std::exchange(offset, 0));
            auto const take = segment.first(std::min(segment.size(), MaxOwnedSend - node.owned.size()));
            node.owned.insert(node.owned.end(), take.begin(), take.end());
            if (node.owned.size() == MaxOwnedSend)
                break;
        }
    }
    node.buffers.assign(1,
                        WSABUF { .len = static_cast<ULONG>(node.owned.size()),
                                 .buf = reinterpret_cast<CHAR*>(node.owned.data()) });
}

void IocpSocket::fillBuffers(Node& node)
{
    // What the cursor still owes, as WSABUFs. A partial send leaves the cursor part-way THROUGH a
    // segment rather than between two, so the first buffer starts at the offset.
    node.buffers.clear();
    if (node.segments.empty())
    {
        auto const left = node.flat.subspan(node.written);
        node.buffers.push_back(
            WSABUF { .len = static_cast<ULONG>(std::min(left.size(), MaxBufferLength)),
                     .buf = const_cast<CHAR*>(reinterpret_cast<CHAR const*>(left.data())) });
        return;
    }
    auto offset = node.segmentOffset;
    for (auto const index: std::views::iota(node.segmentIndex, node.segments.size()))
    {
        auto const segment = node.segments[index].subspan(std::exchange(offset, 0));
        if (segment.empty())
            continue;
        node.buffers.push_back(
            WSABUF { .len = static_cast<ULONG>(std::min(segment.size(), MaxBufferLength)),
                     .buf = const_cast<CHAR*>(reinterpret_cast<CHAR const*>(segment.data())) });
        if (node.buffers.size() == MaxSegmentsPerSend)
            break;
    }
}

void IocpSocket::advanceCursor(Node& node, std::size_t sent) noexcept
{
    node.written += sent;
    auto left = sent;
    while (left > 0 && node.segmentIndex < node.segments.size())
    {
        auto const inThisSegment = node.segments[node.segmentIndex].size() - node.segmentOffset;
        if (left < inThisSegment)
        {
            node.segmentOffset += left;
            return;
        }
        left -= inThisSegment;
        ++node.segmentIndex;
        node.segmentOffset = 0;
    }
}

std::optional<IoResult> IocpSocket::trySend(Node& node) const
{
    while (node.remaining() > 0)
    {
        fillBuffers(node);
        auto sent = DWORD { 0 };
        // Not overlapped: on this non-blocking socket it answers at once, with what the send
        // buffer took or with WSAEWOULDBLOCK.
        if (::WSASend(_socket,
                      node.buffers.data(),
                      static_cast<DWORD>(node.buffers.size()),
                      &sent,
                      0,
                      nullptr,
                      nullptr)
            != 0)
        {
            auto const err = ::WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return std::nullopt;
            return IoResult { std::unexpected(detail::fromWinsockError(err, "WSASend")) };
        }
        if (sent == 0)
            return IoResult { std::unexpected(
                makeNetError(NetErrorCode::SystemError, 0, "WSASend accepted no bytes")) };
        advanceCursor(node, sent);
    }
    return IoResult { node.written };
}

std::optional<IoResult> IocpSocket::advanceWrite(std::shared_ptr<Node> const& node)
{
    auto const error = detail::completionError(_socket, *node);
    if (error != 0)
        return IoResult { std::unexpected(detail::fromWinsockError(static_cast<int>(error), "WSASend")) };

    auto const sent = node->bytesTransferred();
    // A completion that moved nothing on a non-empty send is neither progress nor a named failure,
    // and re-issuing it would spin. Report the one thing that is true.
    if (sent == 0)
        return IoResult { std::unexpected(
            makeNetError(NetErrorCode::SystemError, 0, "WSASend accepted no bytes")) };

    advanceCursor(*node, sent);
    if (node->remaining() == 0)
        return IoResult { node->written };
    // Part of it went, and the flow asked to stop: it is not sent the rest.
    if (node->stopped)
        return IoResult { std::unexpected(cancelled("the awaiting flow was cancelled")) };
    if (auto issued = issueWrite(node); !issued)
        return IoResult { std::unexpected(std::move(issued.error())) };
    return std::nullopt;
}

void IocpSocket::onWriteWake(void* state, ParkWake wake)
{
    auto& node = *static_cast<Node*>(state);
    auto* const socket = node.socket;
    if (node.awaitable == nullptr)
        return;
    // The slot's write, or one orphaned by a second write in a Release build: either way the
    // socket holds it while its park is registered, and this keeps it through a re-issue.
    auto const held = socket->holding(node);
    if (!held)
        return;

    switch (wake)
    {
        case ParkWake::Ready: {
            if (!node.completed)
                return; // a report for the send before a re-issue; this one's follows
            auto done = socket->advanceWrite(held);
            if (!done.has_value())
                return; // re-issued for what a partial completion left
            auto* const awaitable = socket->take(node);
            awaitable->complete(std::move(*done));
            return;
        }
        case ParkWake::Cancelled: {
            node.stopped = true;
            if (node.completed)
            {
                auto done = socket->advanceWrite(held);
                auto* const awaitable = socket->take(node);
                awaitable->complete(done.value_or(
                    IoResult { std::unexpected(cancelled("the awaiting flow was cancelled")) }));
                return;
            }
            // The kernel is still reading the caller's bytes: ask for the send back and resume
            // the flow only when its completion says the kernel is done with them.
            socket->_loop.unregisterPark(std::exchange(node.park, ParkId {}));
            socket->cancelInKernel(node);
            if (auto parked = socket->park(node); !parked)
            {
                auto* const awaitable = socket->take(node);
                awaitable->complete(std::unexpected(cancelled("the awaiting flow was cancelled")));
            }
            return;
        }
        case ParkWake::Abandoned: {
            socket->cancelInKernel(node);
            auto* const awaitable = socket->take(node);
            awaitable->abandon();
            return;
        }
    }
}

void IocpSocket::retireWrite(void* owner, void* awaitable) noexcept
{
    auto* const socket = static_cast<IocpSocket*>(owner);
    // Identity, as on the read side: the slot's write, or an orphan a second write displaced.
    auto node = std::shared_ptr<Node> {};
    if (socket->_write && socket->_write->awaitable == awaitable)
        node = socket->_write;
    else if (auto const found = std::ranges::find_if(
                 socket->_settling, [awaitable](auto const& held) { return held->awaitable == awaitable; });
             found != socket->_settling.end())
        node = *found;
    if (!node)
        return;
    socket->cancelInKernel(*node);
    std::ignore = socket->take(*node);
}

// ---- IocpListener -----------------------------------------------------------------------------

namespace
{
    /// One outstanding `AcceptEx`: the `OVERLAPPED`, and the address block the kernel writes the
    /// two endpoints into. It holds itself while the kernel holds it, exactly as a socket's node
    /// does and for the same reason: the listener, and the accepting frame, may both be gone when
    /// the packet comes back ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)).
    struct AcceptOperation: detail::IocpOperation
    {
        std::array<std::byte, std::size_t { AcceptAddressSize } * 2> addresses {};
        std::shared_ptr<AcceptOperation> self;

        /// @param operation This node.
        static void released(detail::IocpOperation& operation) noexcept
        {
            auto const keep = std::move(static_cast<AcceptOperation&>(operation).self);
        }
    };
} // namespace

/// What an accepting frame needs from its listener AFTER it resumes, shared rather than read
/// through `this`: an accept is a coroutine the caller owns, and the listener may be destroyed
/// while it is parked. Its abort completion then still arrives, and the frame must answer it
/// without touching a listener that is gone.
struct IocpListener::Shared
{
    SOCKET socket = detail::InvalidSocket; ///< The listening socket; `INVALID_SOCKET` once closed.

    /// Every accept parked right now, so a close can resolve them: `IListener::close` says a
    /// pending accept resolves with `Cancelled`, and waiting for each abort to come back through
    /// the port would make that answer depend on the loop turning again -- which a loop being torn
    /// down straight after `close()` does not.
    std::vector<detail::CompletionWait*> accepting;
};

IocpListener::IocpListener(EventLoop& loop,
                           SOCKET socket,
                           int family,
                           std::uint16_t boundPort,
                           void* acceptEx,
                           void* acceptAddresses) noexcept:
    _loop(loop),
    _shared(std::make_shared<Shared>(Shared { .socket = socket, .accepting = {} })),
    _family(family),
    _boundPort(boundPort),
    _acceptEx(acceptEx),
    _acceptAddresses(acceptAddresses)
{
}

IocpListener::~IocpListener()
{
    contract::assertTeardownIsSerialisedWithDispatch(_loop);
    // Closing is the cancellation: a pending AcceptEx completes aborted, into its own node, and
    // the accepting frame answers it from `Shared` without reaching back here.
    close();
}

void IocpListener::close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    // Taken into locals first: resolving an accept resumes a flow that may own this listener and
    // destroy it before the call returns, so nothing below the socket's close reads a member.
    auto const shared = _shared;
    auto const accepting = std::exchange(shared->accepting, {});
    if (shared->socket != detail::InvalidSocket)
    {
        if (auto* const port = _loop.completionPort(); port != nullptr)
            port->forget(reinterpret_cast<platform::NativeHandle>(shared->socket));
        // What aborts every AcceptEx still outstanding; each completes later into its own node.
        ::closesocket(shared->socket);
        shared->socket = detail::InvalidSocket;
    }
    // The socket FILE goes with the socket, as `UnixListener::close` does on POSIX; "" for a TCP listener.
    // DeleteFileA, because this is noexcept.
    if (!_path.empty())
        ::DeleteFileA(_path.c_str());
    for (auto* const wait: accepting)
        wait->close();
}

SOCKET IocpListener::native() const noexcept
{
    return _shared->socket;
}

namespace
{
    /// Prepares a listening socket for this listener: associates it with the port and fetches the
    /// two extension functions an accept needs.
    /// @param loop The loop whose port completes the accepts.
    /// @param socket The listening socket; closed on failure.
    /// @param[out] acceptEx `AcceptEx`.
    /// @param[out] acceptAddresses `GetAcceptExSockaddrs`, or null (the peer address is then "").
    /// @return Nothing, or why the socket cannot be used.
    [[nodiscard]] std::expected<void, NetError> prepareListening(EventLoop& loop,
                                                                 SOCKET socket,
                                                                 LPFN_ACCEPTEX& acceptEx,
                                                                 LPFN_GETACCEPTEXSOCKADDRS& acceptAddresses)
    {
        auto* const port = loop.completionPort();
        if (port == nullptr)
        {
            ::closesocket(socket);
            return std::unexpected(
                makeNetError(NetErrorCode::Unsupported, 0, "the loop's backend lends no completion port"));
        }
        acceptEx = extensionFunction<LPFN_ACCEPTEX>(socket, WSAID_ACCEPTEX);
        if (acceptEx == nullptr)
        {
            auto const err = ::WSAGetLastError();
            ::closesocket(socket);
            return std::unexpected(detail::fromWinsockError(err, "WSAIoctl(AcceptEx)"));
        }
        // Best-effort: without it the peer address is "", which costs a log prefix, not a
        // connection.
        acceptAddresses = extensionFunction<LPFN_GETACCEPTEXSOCKADDRS>(socket, WSAID_GETACCEPTEXSOCKADDRS);
        if (auto associated = port->associate(reinterpret_cast<platform::NativeHandle>(socket)); !associated)
        {
            ::closesocket(socket);
            return std::unexpected(std::move(associated.error()));
        }
        return {};
    }

    /// @param socket A bound socket.
    /// @return Its family and the port the kernel gave it.
    [[nodiscard]] std::pair<int, std::uint16_t> boundEndpointOf(SOCKET socket) noexcept
    {
        auto bound = sockaddr_storage {};
        auto length = int { sizeof(bound) };
        if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0)
            return { AF_INET, std::uint16_t { 0 } };
        return { bound.ss_family, detail::portOfSockaddr(&bound, static_cast<std::uint32_t>(length)) };
    }
} // namespace

std::expected<std::unique_ptr<IocpListener>, NetError> IocpListener::bind(EventLoop& loop,
                                                                          std::string_view host,
                                                                          std::uint16_t port,
                                                                          int backlog,
                                                                          SocketBufferSizes acceptedBuffers)
{
    platform::ensureWinsockInitialized();

    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    auto const hostString = std::string { host };
    auto const portString = std::to_string(port);
    addrinfo* resolved = nullptr;
    if (auto const rc = ::getaddrinfo(
            hostString.empty() ? nullptr : hostString.c_str(), portString.c_str(), &hints, &resolved);
        rc != 0 || resolved == nullptr)
        return std::unexpected(makeNetError(NetErrorCode::AddressError, rc, "getaddrinfo"));

    auto socket = detail::InvalidSocket;
    auto lastError = makeNetError(NetErrorCode::AddressError, 0, "no usable address");
    auto const* next = resolved;
    while (next != nullptr)
    {
        auto const* candidate = std::exchange(next, next->ai_next);
        socket = ::WSASocketW(candidate->ai_family,
                              candidate->ai_socktype,
                              candidate->ai_protocol,
                              nullptr,
                              0,
                              WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (socket == detail::InvalidSocket)
        {
            lastError = detail::fromWinsockError(::WSAGetLastError(), "socket");
            continue;
        }
        // Before listen, so the window scale of every connection it accepts can count the receive
        // buffer. The listener's, not the AcceptEx socket's: an accepted socket ends up with the
        // listener's sizes, whatever its AcceptEx socket was created with -- measured, with the
        // sizes asked of the AcceptEx socket alone an accepted one read back 65536.
        detail::applySocketBufferSizes(reinterpret_cast<platform::NativeHandle>(socket), acceptedBuffers);
        // Exclusive, and a failure to make it so fails the bind rather than being ignored: this
        // option is a security property, not a tuning one.
        auto const exclusive = BOOL { TRUE };
        if (::setsockopt(socket,
                         SOL_SOCKET,
                         SO_EXCLUSIVEADDRUSE,
                         reinterpret_cast<char const*>(&exclusive),
                         static_cast<int>(sizeof(exclusive)))
                == 0
            && ::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0
            && ::listen(socket, backlog) == 0)
            break;
        lastError = detail::fromWinsockError(::WSAGetLastError(), "bind/listen");
        ::closesocket(socket);
        socket = detail::InvalidSocket;
    }
    ::freeaddrinfo(resolved);
    if (socket == detail::InvalidSocket)
        return std::unexpected(std::move(lastError));

    auto acceptEx = LPFN_ACCEPTEX { nullptr };
    auto acceptAddresses = LPFN_GETACCEPTEXSOCKADDRS { nullptr };
    if (auto prepared = prepareListening(loop, socket, acceptEx, acceptAddresses); !prepared)
        return std::unexpected(std::move(prepared.error()));

    auto const [family, boundPort] = boundEndpointOf(socket);
    return std::unique_ptr<IocpListener> { new IocpListener(loop,
                                                            socket,
                                                            family,
                                                            boundPort,
                                                            reinterpret_cast<void*>(acceptEx),
                                                            reinterpret_cast<void*>(acceptAddresses)) };
}

std::expected<std::unique_ptr<IocpListener>, NetError> IocpListener::bindUnix(EventLoop& loop,
                                                                              std::string_view path,
                                                                              int backlog)
{
    platform::ensureWinsockInitialized();
    auto const claimed = detail::claimUnixSocketPath(path);
    if (!claimed)
        return std::unexpected(claimed.error());

    auto const socket =
        ::WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == detail::InvalidSocket)
        return std::unexpected(
            makeNetError(NetErrorCode::Unsupported, ::WSAGetLastError(), "socket(AF_UNIX)"));
    if (::bind(socket, reinterpret_cast<sockaddr const*>(&*claimed), static_cast<int>(sizeof(*claimed))) != 0)
    {
        auto const err = ::WSAGetLastError();
        ::closesocket(socket);
        return std::unexpected(makeNetError(
            err == WSAEADDRINUSE ? NetErrorCode::AddressInUse : NetErrorCode::SystemError, err, "bind unix"));
    }

    // Bound, so the socket FILE exists now, and it is this call's: every way out below that does
    // not hand it to a listener -- whose close deletes it -- deletes it here, or the next bind of
    // the path finds a stale file to reclaim for a listener that never existed.
    auto const pathString = std::string { path };
    if (::listen(socket, backlog) != 0)
    {
        auto const err = ::WSAGetLastError();
        ::closesocket(socket);
        ::DeleteFileA(pathString.c_str());
        return std::unexpected(makeNetError(NetErrorCode::SystemError, err, "listen unix"));
    }

    auto acceptEx = LPFN_ACCEPTEX { nullptr };
    auto acceptAddresses = LPFN_GETACCEPTEXSOCKADDRS { nullptr };
    if (auto prepared = prepareListening(loop, socket, acceptEx, acceptAddresses); !prepared)
    {
        ::DeleteFileA(pathString.c_str()); // `prepareListening` has closed the socket
        return std::unexpected(std::move(prepared.error()));
    }

    auto listener =
        std::unique_ptr<IocpListener> { new IocpListener(loop,
                                                         socket,
                                                         AF_UNIX,
                                                         /*boundPort=*/0,
                                                         reinterpret_cast<void*>(acceptEx),
                                                         reinterpret_cast<void*>(acceptAddresses)) };
    listener->_path = pathString;
    return listener;
}

std::expected<std::unique_ptr<IocpListener>, NetError> IocpListener::adopt(EventLoop& loop, SOCKET socket)
{
    platform::ensureWinsockInitialized();
    if (socket == detail::InvalidSocket)
        return std::unexpected(makeNetError(NetErrorCode::BadHandle, 0, "adoptListener"));

    auto acceptEx = LPFN_ACCEPTEX { nullptr };
    auto acceptAddresses = LPFN_GETACCEPTEXSOCKADDRS { nullptr };
    if (auto prepared = prepareListening(loop, socket, acceptEx, acceptAddresses); !prepared)
        return std::unexpected(std::move(prepared.error()));

    // Asked of the KERNEL: the caller adopting a socket is exactly the caller that does not know
    // which port it is.
    auto const [family, boundPort] = boundEndpointOf(socket);
    return std::unique_ptr<IocpListener> { new IocpListener(loop,
                                                            socket,
                                                            family,
                                                            boundPort,
                                                            reinterpret_cast<void*>(acceptEx),
                                                            reinterpret_cast<void*>(acceptAddresses)) };
}

async::Task<AcceptResult> IocpListener::accept()
{
    // Everything the frame needs after it resumes is copied out of `this` NOW: the listener may be
    // destroyed while the accept is parked, and its abort completion still arrives here.
    auto* const loop = &_loop;
    auto const shared = _shared;
    auto const listening = shared->socket;
    if (_closed || listening == detail::InvalidSocket)
        co_return std::unexpected(cancelled("accept on closed listener"));
    auto* const port = loop->completionPort();
    auto* const acceptEx = reinterpret_cast<LPFN_ACCEPTEX>(_acceptEx);
    auto* const acceptAddresses = reinterpret_cast<LPFN_GETACCEPTEXSOCKADDRS>(_acceptAddresses);

    // AF_UNIX takes protocol 0; IPPROTO_TCP is refused for it.
    auto const protocol = _family == AF_UNIX ? 0 : IPPROTO_TCP;
    auto accepted = ::WSASocketW(
        _family, SOCK_STREAM, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (accepted == detail::InvalidSocket)
        co_return std::unexpected(detail::fromWinsockError(::WSAGetLastError(), "socket(accept)"));
    // Closed on every way out but the hand-off to `IocpSocket` -- a frame destroyed while parked
    // included, which no line below would otherwise see. Closing it is what aborts an AcceptEx
    // nothing will wait for, whose completion lands in the operation's own share; left open, it
    // would take the next client into a socket nobody owns.
    auto const discard = detail::ScopeGuard { [&accepted]() noexcept {
        if (accepted != detail::InvalidSocket)
            ::closesocket(accepted);
    } };

    auto operation = std::make_shared<AcceptOperation>();
    operation->onDequeued = &AcceptOperation::released;
    port->beginOperation(static_cast<detail::IocpOperation*>(operation.get()));
    operation->self = operation;
    auto received = DWORD { 0 };
    if (acceptEx(listening,
                 accepted,
                 operation->addresses.data(),
                 0,
                 AcceptAddressSize,
                 AcceptAddressSize,
                 &received,
                 &operation->overlapped)
        == FALSE)
    {
        auto const err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            port->withdrawOperation(static_cast<detail::IocpOperation*>(operation.get()));
            operation->self.reset();
            co_return std::unexpected(detail::fromWinsockError(err, "AcceptEx"));
        }
    }

    auto wait = detail::CompletionWait { *loop, *operation, listening };
    shared->accepting.push_back(&wait);
    // Delisted however the frame leaves -- resumed, or destroyed while parked by whoever owns it --
    // so a later close() never reaches a wait that is gone.
    auto const listed =
        detail::ScopeGuard { [&shared, &wait]() noexcept { std::erase(shared->accepting, &wait); } };
    co_await wait;

    if (wait.outcome() != detail::CompletionWait::Outcome::Completed)
    {
        switch (wait.outcome())
        {
            case detail::CompletionWait::Outcome::Refused: co_return std::unexpected(parkRefused());
            case detail::CompletionWait::Outcome::Closed:
                co_return std::unexpected(cancelled("the listener was closed"));
            default: co_return std::unexpected(cancelled("the event loop is going away"));
        }
    }

    // Asked of the LISTENING socket, on which the operation was issued, unless it has been closed
    // since -- which is then what aborted it.
    auto const error = detail::completionError(shared->socket, *operation);
    if (error != 0)
        co_return std::unexpected(detail::fromWinsockError(static_cast<int>(error), "AcceptEx"));

    // Without this the socket is connected and yet `shutdown` fails on it with WSAENOTCONN:
    // AcceptEx leaves the handle's context unset until asked, so a server that half-closed sent no
    // FIN (fastcached#1556). Best-effort, because a failed accept would end an accept loop over a
    // socket that still reads and writes.
    std::ignore = ::setsockopt(accepted,
                               SOL_SOCKET,
                               SO_UPDATE_ACCEPT_CONTEXT,
                               reinterpret_cast<char const*>(&listening),
                               static_cast<int>(sizeof(listening)));

    // What a dialled socket is given too -- TCP_NODELAY above all, which only the dial used to set.
    // After the context update, which is what makes the socket answer as a connected TCP socket.
    detail::applyStreamSocketOptions(reinterpret_cast<platform::NativeHandle>(accepted), KeepAlive::No);

    // AcceptEx wrote both endpoints into the block; this parses the peer out with no syscall.
    auto peer = std::string {};
    if (acceptAddresses != nullptr)
    {
        sockaddr* local = nullptr;
        sockaddr* remote = nullptr;
        auto localLength = 0;
        auto remoteLength = 0;
        acceptAddresses(operation->addresses.data(),
                        0,
                        AcceptAddressSize,
                        AcceptAddressSize,
                        &local,
                        &localLength,
                        &remote,
                        &remoteLength);
        if (remote != nullptr && remoteLength > 0
            && static_cast<std::size_t>(remoteLength) <= sizeof(sockaddr_storage))
        {
            auto storage = sockaddr_storage {};
            std::memcpy(&storage, remote, static_cast<std::size_t>(remoteLength));
            peer = formatPeer(storage);
        }
    }
    co_return std::unique_ptr<ISocket> { new IocpSocket(
        *loop, std::exchange(accepted, detail::InvalidSocket), std::move(peer)) };
}

} // namespace core::net
