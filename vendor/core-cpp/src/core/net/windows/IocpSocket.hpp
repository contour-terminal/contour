// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IocpSocket` and `IocpListener` — the Windows socket and listener whose operations are
/// overlapped `WSARecv`, `WSASend` and `AcceptEx` completed by the loop's completion port.
///
/// **What makes them different from every other socket in this library is who writes last.** A
/// readiness socket (`PosixSocket`) is told that a syscall would now succeed and
/// then performs it, so it decides the outcome itself at the moment it is resumed. Here the KERNEL
/// performs the operation and the completion reports what it did — so a cancel, a deadline or a
/// stop cannot settle an operation on its own; it asks the kernel for the operation back
/// (`CancelIoEx`) and the completion that follows is the answer. That is how bytes already
/// received win over a later stop
/// ([fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)).
///
/// **The kernel is never handed the caller's memory.** An overlapped receive lands in a buffer the
/// operation owns and is copied out when its completion reaches a flow that is still waiting; an
/// overlapped send is copied in when it is issued. Every abandon path -- an awaitable destroyed
/// while parked, a socket destroyed under one, a loop torn down -- ends the caller's borrow before
/// the kernel is done, and `CancelIoEx` only asks; without the copy, a receive the peer's data
/// reached first would complete into freed memory. Data already there, and room already in the
/// send buffer, are still moved synchronously to and from the caller's memory, with no copy.
///
/// **The kernel holds a pointer into an operation node, never into the socket**
/// ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)). Each node owns
/// one share of itself while the kernel holds it, given back when the port dequeues its packet, so
/// a socket destroyed with a receive or a send in flight leaves the kernel writing into — and
/// reading a gathered write's payload out of — storage that still exists.
///
/// A completion reaches its socket as readiness reaches every other one: the node is parked on the
/// loop (@c HandleKind::Completion), the backend reports it, and the loop runs the socket's
/// callback in turn step 2 (`windows/IocpOperation.hpp` says why that and not a callback from the
/// port).
///
/// **Bytes already there, and room already in the send buffer, cost no operation.** Each read and
/// write is tried once without one first (the socket is non-blocking, which an overlapped
/// operation ignores -- libuv runs its Windows sockets the same way), so a read of pending data or a
/// write that fits returns inline and a caller's turn count is the same on every platform. Only
/// what would block becomes an overlapped operation.
///
/// Upstream: fastcached `Net/IocpSocket.{hpp,cpp}` at `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`,
/// rewritten onto `ISocket`'s frame-free, stop-aware awaitables.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/SocketBuffers.hpp>

// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::net
{

/// Whether a handle handed to @c IocpSocket has already been associated with the loop's
/// completion port.
///
/// `ConnectEx` requires the association BEFORE the operation is issued, so a dial associates the
/// handle itself; a second association of one handle is refused (G4), and a socket that asked for
/// one anyway would report a working connection as unusable. An `enum class` rather than a `bool`
/// so the call site says which (`.agent/rules/design-principles.md`).
enum class IocpAssociation : std::uint8_t
{
    Associate,         ///< The constructor associates the handle.
    AlreadyAssociated, ///< The caller did; the constructor must not repeat it.
};

/// A connected stream socket whose reads and writes are overlapped operations on the loop's
/// completion port.
///
/// **`cancelRead` on a real read SETTLES rather than resolving inline, and that is a platform
/// fact rather than a choice**
/// ([fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)). `CancelIoEx` cannot
/// un-receive: a receive that has already completed has taken its bytes out of the stream and written them
/// into the caller's buffer, and nothing at the moment of the call can tell it apart from one still pending —
/// `WSAGetOverlappedResult` answers `WSA_IO_INCOMPLETE` for both until the packet is dequeued (measured
/// upstream). So the retired read resolves on a later turn with whatever its operation did: its bytes, or @c
/// NetErrorCode::Cancelled from the abort. What IS synchronous is the slot — a read armed right after
/// `cancelRead` gets its own operation, never the one the kernel still owns. A `waitReadable` probe is a
/// ZERO-byte receive, carries nothing to lose, and is retired inline as the interface describes.
class IocpSocket final: public ISocket
{
  public:
    /// Wraps a connected socket and associates it with @p loop's completion port.
    ///
    /// A socket the port would not take is still constructed — a constructed object is usable,
    /// if only to be told why — and every operation on it answers the association's refusal.
    /// @param loop The loop whose backend's port completes this socket's operations (not owned).
    ///        Its backend MUST lend a completion port.
    /// @param socket The connected SOCKET (ownership transferred).
    /// @param peerAddress Printable peer address, or "" if unknown.
    /// @param association Whether the caller has already associated @p socket with the port.
    IocpSocket(EventLoop& loop,
               SOCKET socket,
               std::string peerAddress = {},
               IocpAssociation association = IocpAssociation::Associate) noexcept;
    ~IocpSocket() override;

    IocpSocket(IocpSocket const&) = delete;
    IocpSocket& operator=(IocpSocket const&) = delete;
    IocpSocket(IocpSocket&&) = delete;
    IocpSocket& operator=(IocpSocket&&) = delete;

    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;

    /// @copydoc ISocket::waitReadable
    ///
    /// A zero-byte `WSARecv` completes when the socket is readable and reports zero bytes whether
    /// a megabyte is waiting or the peer has gone, so the count is MEASURED once it completes: one
    /// `MSG_PEEK` of one byte, which consumes nothing — fastcached's `Net/IocpSocket.cpp:327-334`,
    /// and the reason [fastcached#677](https://github.com/LASTRADA-Software/fastcached/issues/677)
    /// stopped reporting opposite numbers on Windows and Linux for the same event.
    [[nodiscard]] IoAwaitable waitReadable() override;

    /// @copydoc ISocket::cancelRead
    ///
    /// See the class comment for why a real read settles rather than resolving inline.
    void cancelRead() noexcept override;

    /// @copydoc ISocket::shutdownWrite
    ///
    /// Completes inline: `shutdown(SD_SEND)` either takes or it does not. The precondition — no
    /// write outstanding — is the interface's and the caller's; like every other plain socket this
    /// one does not assert it, because the socket contract's write-slot canary breaks it on purpose
    /// to reach the guard it exists to watch.
    [[nodiscard]] ResultAwaitable<void> shutdownWrite() override;

    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override;

    [[nodiscard]] std::string peerAddress() const override { return _peerAddress; }

    void close() noexcept override;

    /// @return True once @c close was called, or a read observed the peer's EOF; @see
    ///         ISocket::isClosed.
    [[nodiscard]] bool isClosed() const noexcept override { return _closed || _peerClosed; }

    /// @return The SOCKET, or `INVALID_SOCKET` once closed (for diagnostics and tests).
    [[nodiscard]] SOCKET native() const noexcept { return _socket; }

    /// One overlapped operation of this socket. Defined in the implementation; named here so the
    /// socket can hold them.
    struct Node;

  private:
    /// Closes the socket, taking every parked operation back first.
    /// @param policy @c FdWakePolicy::Resume completes a parked operation with a
    ///        @c NetErrorCode::Cancelled VALUE (@c close); @c FdWakePolicy::Cancel abandons it so
    ///        the flow unwinds (the destructor).
    void close(FdWakePolicy policy) noexcept;

    /// Issues @p node's receive and parks it on the loop.
    /// @param node The read to issue.
    /// @return Nothing, or why it could not be issued.
    [[nodiscard]] std::expected<void, NetError> issueRead(std::shared_ptr<Node> const& node);

    /// Issues @p node's send of whatever its cursor still owes, parking it on the loop the first
    /// time.
    /// @param node The write to issue.
    /// @return Nothing, or why it could not be issued.
    [[nodiscard]] std::expected<void, NetError> issueWrite(std::shared_ptr<Node> const& node);

    /// @param node An operation of this socket's.
    /// @return The share this socket holds of @p node, from whichever slot it is in; empty if none.
    [[nodiscard]] std::shared_ptr<Node> holding(Node const& node) const noexcept;

    /// Parks @p node on the loop, so its completion reaches this socket.
    /// @param node The operation to park.
    /// @return Nothing, or the loop's refusal.
    [[nodiscard]] std::expected<void, NetError> park(Node& node);

    /// Hands @p node to the kernel through @p issue, keeping it alive until the port dequeues it.
    /// @param node The operation.
    /// @param issue The Winsock call; answers 0 or the error, `WSA_IO_PENDING` included.
    /// @param what The call's name, for the error.
    /// @return Nothing, or why the kernel refused it outright.
    template <typename Issue>
    [[nodiscard]] std::expected<void, NetError> handToKernel(std::shared_ptr<Node> const& node,
                                                             Issue issue,
                                                             char const* what);

    /// Takes @p node out of this socket: its park, its deadline, and whichever slot holds it.
    /// @param node The operation to take.
    /// @return The awaitable parked on it, for the caller to settle AFTERWARDS.
    [[nodiscard]] IoAwaitable* take(Node& node) noexcept;

    /// Asks the kernel for @p node back; the completion still arrives.
    /// @param node The operation to cancel.
    void cancelInKernel(Node& node) const noexcept;

    /// What a completed read resolves with.
    /// @param node The dequeued read.
    /// @return The byte count (a probe's `0`/`1`), or the classified error.
    [[nodiscard]] IoResult readResult(Node& node);

    /// Attempts a non-overlapped receive: what is already here is taken with no operation.
    /// @param buffer The destination.
    /// @return The count, `0` on EOF, an error, or nullopt when the read would block.
    [[nodiscard]] std::optional<IoResult> tryReceive(std::span<std::byte> buffer);

    /// One `MSG_PEEK` of one byte: `0` for EOF, `1` for pending data, consuming nothing.
    /// @return The count, an error, or nullopt when nothing is there yet.
    [[nodiscard]] std::optional<IoResult> tryPeek() const;

    /// Sends what @p node still owes without an operation, as far as the send buffer takes it.
    /// @param node A write, not yet parked.
    /// @return The total once every byte is gone, an error, or nullopt when the rest would block.
    [[nodiscard]] std::optional<IoResult> trySend(Node& node) const;

    /// Copies what @p node still owes, up to a bound, into its own buffer, and points its one
    /// `WSABUF` at the copy: an overlapped send never reads the caller's memory.
    /// @param node The write.
    static void copyOwed(Node& node);

    /// Rebuilds @p node's `WSABUF`s from its cursor, over the CALLER's memory -- for the
    /// synchronous attempt only, which is finished with it before the verb returns.
    /// @param node The write.
    static void fillBuffers(Node& node);

    /// Moves @p node's cursor past @p sent bytes, which may leave it part-way through a segment.
    /// @param node The write.
    /// @param sent How many bytes the kernel took.
    static void advanceCursor(Node& node, std::size_t sent) noexcept;

    /// Moves a completed write's cursor, re-issuing if the kernel took only part of it.
    /// @param node The dequeued write.
    /// @return The total once every byte is gone, an error, or nullopt when it was re-issued.
    [[nodiscard]] std::optional<IoResult> advanceWrite(std::shared_ptr<Node> const& node);

    /// @param state The node, as a `void*`.
    /// @param wake Why the park woke.
    static void onReadWake(void* state, ParkWake wake);

    /// @param state The node, as a `void*`.
    /// @param wake Why the park woke.
    static void onWriteWake(void* state, ParkWake wake);

    /// @param state The node, as a `void*`. Runs when a read's receive deadline elapses.
    static void onReadDeadline(void* state);

    /// @param owner The socket, as a `void*`.
    /// @param awaitable The awaitable going away.
    static void retireRead(void* owner, void* awaitable) noexcept;

    /// @param owner The socket, as a `void*`.
    /// @param awaitable The awaitable going away.
    static void retireWrite(void* owner, void* awaitable) noexcept;

    /// @return The error every verb reports when the socket could not be used at all.
    [[nodiscard]] std::optional<NetError> unusable(char const* what) const;

    EventLoop& _loop;
    ICompletionPort* _port;
    SOCKET _socket;
    std::string _peerAddress;

    /// Why the port would not take this socket, if it would not.
    std::optional<NetError> _associationError;

    std::shared_ptr<Node> _read;  ///< The read slot: the operation a read verb last armed.
    std::shared_ptr<Node> _write; ///< The write slot.

    /// Reads `cancelRead` retired while their operation was still the kernel's, and -- in a
    /// Release build only -- a parked write a second write displaced: each resolves with whatever
    /// its completion says, and is held here so that close and the destructor can still reach its
    /// waiter.
    std::vector<std::shared_ptr<Node>> _settling;

    /// How long a single read may wait, or zero for no bound; @see ISocket::setReceiveDeadline.
    std::chrono::milliseconds _receiveDeadline { 0 };

    bool _closed = false;
    bool _peerClosed = false; ///< Latched by a read that observed EOF; only @c isClosed reads it.
};

/// A listener whose accepts are overlapped `AcceptEx` operations on the loop's completion port,
/// handing out @c IocpSocket.
class IocpListener final: public IListener
{
  public:
    ~IocpListener() override;

    IocpListener(IocpListener const&) = delete;
    IocpListener& operator=(IocpListener const&) = delete;
    IocpListener(IocpListener&&) = delete;
    IocpListener& operator=(IocpListener&&) = delete;

    /// Binds and listens on @p host : @p port, claiming the address exclusively
    /// (`SO_EXCLUSIVEADDRUSE`; `.agent/rules/async-and-net.md`, "A listening socket claims its
    /// address exclusively").
    /// @param loop The loop whose backend's port completes the accepts (not owned).
    /// @param host The bind address; empty is the wildcard.
    /// @param port The bind port; 0 requests an ephemeral one.
    /// @param backlog The listen backlog.
    /// @param acceptedBuffers The kernel buffer sizes, asked of the listening socket before it
    ///        listens; every socket it accepts takes them from it.
    /// @return The listener, or why it could not be bound.
    [[nodiscard]] static std::expected<std::unique_ptr<IocpListener>, NetError> bind(
        EventLoop& loop,
        std::string_view host,
        std::uint16_t port,
        int backlog = 128,
        SocketBufferSizes acceptedBuffers = {});

    /// Binds and listens on the AF_UNIX socket file @p path, claiming it as `UnixListener`
    /// does on POSIX: a stale socket file is reclaimed, a live server's is refused, anything else at the
    /// path is never touched. The file goes with the listener when it closes.
    ///
    /// **AcceptEx accepts AF_UNIX connections**, measured on Windows 11 (26200) and asserted by
    /// `windows/UnixListener_test.cpp` on every CI run, so an IOCP loop serves a unix socket
    /// through its completion port like any other listener, and hands out @c IocpSocket.
    /// @param loop The loop whose backend's port completes the accepts (not owned).
    /// @param path The socket file path.
    /// @param backlog The listen backlog.
    /// @return The listener, or why the path could not be bound.
    [[nodiscard]] static std::expected<std::unique_ptr<IocpListener>, NetError> bindUnix(
        EventLoop& loop, std::string_view path, int backlog = 128);

    /// Adopts an already-bound, already-listening socket; @see core::net::adoptListener.
    /// @param loop The loop whose backend's port completes the accepts (not owned).
    /// @param socket The listening socket; ownership transfers.
    /// @return The listener, or why the socket could not be prepared.
    [[nodiscard]] static std::expected<std::unique_ptr<IocpListener>, NetError> adopt(EventLoop& loop,
                                                                                      SOCKET socket);

    /// @copydoc IListener::accept
    ///
    /// One `AcceptEx` per call, awaited while it is outstanding: an accept that is not awaited
    /// drops a connection the kernel has already taken off the backlog.
    [[nodiscard]] async::Task<AcceptResult> accept() override;

    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return _boundPort; }

    /// Closes the listening socket, which aborts a pending `AcceptEx`: that accept resolves with
    /// @c NetErrorCode::Cancelled when its completion arrives.
    void close() noexcept override;

    /// @return The listening socket, or `INVALID_SOCKET` once closed; for diagnostics and tests.
    [[nodiscard]] SOCKET native() const noexcept;

    /// What a parked accept reads after it resumes; defined in the implementation.
    struct Shared;

  private:
    /// @param loop The loop whose port completes the accepts.
    /// @param socket The listening socket, already associated with the port.
    /// @param family Its address family, which every accepted socket must share.
    /// @param boundPort The port the kernel gave it.
    /// @param acceptEx `AcceptEx`, as an opaque pointer so this header need not name `mswsock.h`.
    /// @param acceptAddresses `GetAcceptExSockaddrs`, likewise; may be null.
    IocpListener(EventLoop& loop,
                 SOCKET socket,
                 int family,
                 std::uint16_t boundPort,
                 void* acceptEx,
                 void* acceptAddresses) noexcept;

    EventLoop& _loop;
    std::shared_ptr<Shared> _shared; ///< The listening socket, shared with every parked accept.
    int _family;
    std::uint16_t _boundPort;
    void* _acceptEx;
    void* _acceptAddresses;
    std::string _path; ///< The socket file of an AF_UNIX listener, deleted on close; "" for TCP.
    bool _closed = false;
};

} // namespace core::net
