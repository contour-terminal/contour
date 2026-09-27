// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Cross-platform factory functions for the async socket layer. Consumers use
/// these instead of including the per-platform implementation headers directly;
/// each resolves to the right transport (PosixSocket/Listener or
/// IocpSocket/Listener) at compile time.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/SocketBuffers.hpp>
#include <core/net/UdpSocket.hpp>
#include <core/platform/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core::net
{

/// What one `listen` asks for.
///
/// **A named descriptor rather than positional parameters**, for the reason @c DialOptions gives:
/// the bind side is already three values, two of them integers, and a caller that transposes
/// `port` and `backlog` gets a listener on a port it did not choose with no diagnostic at all.
/// It is also the shape the design spec's rename map asks for, in place of each platform
/// listener's own `Bind`.
struct ListenOptions
{
    /// The bind address: "127.0.0.1", "0.0.0.0", "::". Empty means the wildcard address.
    std::string_view host = {};

    /// The bind port; 0 requests an OS-assigned ephemeral one, which
    /// @c IListener::boundPort then reports.
    std::uint16_t port = 0;

    /// The `::listen` backlog.
    int backlog = 128;

    /// Whether other listeners may bind the same address and port at the same time.
    ///
    /// @c PortSharing::Shared lets several listeners bind the same address and port at once. Whether
    /// the kernel also spreads the connections across them depends on the platform:
    ///
    /// - **Linux** spreads them (`SO_REUSEPORT`, by a hash of each connection's addresses), which is
    ///   what a server with one listener per loop asks for.
    /// - **FreeBSD** spreads them (`SO_REUSEPORT_LB`, used wherever the constant is defined).
    /// - **macOS and the other BSDs** bind, and spread nothing: the newest listener gets every
    ///   connection (`SO_REUSEPORT`). A server there that binds one listener per loop leaves all
    ///   but one loop idle; accepting on one loop and handing each socket to another with
    ///   @c adoptSocket is what spreads the work.
    ///
    /// On Windows it is refused with @c NetErrorCode::Unsupported, never mapped: Windows has no
    /// such option, and its `SO_REUSEADDR` lets a later socket take over a port another one holds,
    /// which is a hijack rather than a share. The default is exclusive, so a second bind of a held
    /// port fails with @c NetErrorCode::AddressInUse.
    PortSharing sharing = PortSharing::Exclusive;

    /// The kernel send and receive buffers of every socket this listener accepts; unset ones keep
    /// the kernel's value. They are asked of the listening socket before it listens, and what it
    /// accepts inherits them. See @c SocketBufferSizes for what a size does and does not promise.
    SocketBufferSizes buffers {};
};

/// Binds a TCP listener as @p options asks, driven by @p loop's backend.
/// @param loop The loop whose backend drives accept readiness (not owned).
/// @param options The bind address, port and backlog.
/// @return The bound listener, or a @c NetError on failure.
[[nodiscard]] std::expected<std::unique_ptr<IListener>, NetError> listen(EventLoop& loop,
                                                                         ListenOptions options);

/// Binds a TCP listener on @p host : @p port.
///
/// The positional spelling, kept because it is what contour's callers write. It forwards to the
/// @c ListenOptions overload, which is the one a new caller should use.
/// @param loop The loop whose backend drives accept readiness (not owned).
/// @param host The bind address ("127.0.0.1", "0.0.0.0", "::").
/// @param port The bind port; 0 requests an OS-assigned ephemeral port.
/// @param backlog The listen backlog.
/// @return The bound listener, or a @c NetError on failure.
[[nodiscard]] std::expected<std::unique_ptr<IListener>, NetError> listen(EventLoop& loop,
                                                                         std::string_view host,
                                                                         std::uint16_t port,
                                                                         int backlog = 128);

/// Adopts an already-bound, already-listening handle as an @c IListener driven by @p loop.
///
/// For a socket this process did not create: one inherited from a supervisor (systemd socket
/// activation passes descriptor 3), or one a test bound for itself. On success, ownership of
/// @p handle transfers to the returned listener, which closes it. **On failure the caller still
/// owns @p handle** and closes it itself: nothing was adopted, so nothing here closed it. The
/// same on every platform — a caller that could not tell which would either leak the handle or
/// close it twice.
///
/// **It does not bind and does not listen.** A handle that is merely open, or open and bound but
/// not listening, is adopted successfully and then accepts nothing — the kernel is the only thing
/// that knows, and neither platform offers a portable way to ask.
/// @param loop The loop whose backend drives accept readiness (not owned).
/// @param handle The listening socket handle (a descriptor on POSIX, a `SOCKET` on Windows).
/// @return The adopted listener, or a @c NetError if the handle could not be prepared.
[[nodiscard]] std::expected<std::unique_ptr<IListener>, NetError> adoptListener(
    EventLoop& loop, platform::NativeHandle handle);

/// Connects a TCP client socket to @p host : @p port, parking the caller until the connection
/// completes.
///
/// **Name resolution does not run on @p loop's thread.** It goes to @c defaultAsyncResolver,
/// whose pool starts on first use and is never touched by a dial to a literal address. This is
/// the behaviour change contour's callers inherit: the call looks the same and no longer stalls
/// every other coroutine on the loop for the length of a DNS lookup.
///
/// **A stop of the awaiting flow's own token throws** @c async::OperationCancelled, where
/// contour's body caught it and returned @c NetErrorCode::Cancelled. That is the rule every loop
/// awaitable follows — a cancel from the FLOW unwinds, a cancel from the RESOURCE is a value — and
/// @c connectUnix, below, does not follow it yet.
/// @param loop The loop whose backend drives connect readiness (not owned; a pointer, since
///        coroutine reference parameters can dangle).
/// @param host The remote host ("127.0.0.1", a hostname), unbracketed. **By value**: this is a
///        lazy coroutine, whose body — and so any copy it makes — does not run until the task is
///        first awaited, so a `string_view` would name storage the caller may have destroyed by
///        then. `connect(loop, makeHost(), port)` is the ordinary call that would dangle.
/// @param port The remote port.
/// @return A task resolving to the connected socket, or a @c NetError on failure: a name that
///         cannot be resolved is @c NetErrorCode::AddressError.
/// @throws async::OperationCancelled if the awaiting flow is stopped while the dial is outstanding.
[[nodiscard]] async::Task<SocketResult> connect(EventLoop* loop, std::string host, std::uint16_t port);

/// Connects through an injected resolver, with a per-call budget and keepalive.
///
/// The form to use where the process resolver is not what you want: a test that must observe
/// which thread resolved, a consumer with its own cache, a caller that needs the dial bounded.
/// @param loop The loop whose backend drives connect readiness (not owned).
/// @param host The remote host, unbracketed. By value, for the reason the overload above gives.
/// @param port The remote port.
/// @param resolver The name-resolution seam (not owned; a pointer, since coroutine reference
///        parameters can dangle — the same reason @p loop is one). Must outlive the returned task.
/// @param options The budget, which covers resolution as well as the dial, and whether the
///        connection carries keepalive.
/// @return A task resolving to the connected socket, or a @c NetError on failure;
///         @c NetErrorCode::Timeout when the budget ran out.
/// @throws async::OperationCancelled if the awaiting flow is stopped while the dial is outstanding.
[[nodiscard]] async::Task<SocketResult> connect(EventLoop* loop,
                                                std::string host,
                                                std::uint16_t port,
                                                IAsyncAddressResolver* resolver,
                                                DialOptions options);

/// Binds an AF_UNIX listener on the socket file @p path, hardening its parent
/// directory first (see UnixListener::bind for the exact policy).
/// @param loop The loop whose reactor drives accept readiness (not owned).
/// @param path The socket file path.
/// @param backlog The listen backlog.
/// On Windows the listener is an @c IocpListener handing out @c IocpSocket, as @c listen's is,
/// and a loop whose backend lends no completion port is refused with
/// @c NetErrorCode::Unsupported (since 0.5.0).
/// @return The bound listener; @c NetErrorCode::Unsupported where AF_UNIX is not available.
[[nodiscard]] std::expected<std::unique_ptr<IListener>, NetError> listenUnix(EventLoop& loop,
                                                                             std::string_view path,
                                                                             int backlog = 128);

/// Connects to the AF_UNIX socket file @p path.
///
/// **Unlike @c connect, a stop of the awaiting flow is returned as @c NetErrorCode::Cancelled
/// rather than thrown.** That is contour's behaviour, kept: this dial resolves no name and does
/// not go through @c detail::dialReadiness, and moving it onto that dial — which is where it
/// would pick up the throwing rule — is Task B9's. A caller that stops a flow awaiting both
/// kinds of dial handles both answers until then.
/// @param loop The loop whose reactor drives connect readiness (not owned; a
///        pointer, since coroutine reference parameters can dangle).
/// @param path The socket file path.
/// @return A task resolving to the connected socket -- on Windows an @c IocpSocket --; a
///         @c NetError on failure,
///         @c NetErrorCode::Cancelled if the flow was stopped while it waited,
///         @c NetErrorCode::Unsupported where AF_UNIX is not available.
[[nodiscard]] async::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectUnix(
    EventLoop* loop, std::string_view path);

/// Adopts an already-open stream file descriptor (a socketpair end, a PTY
/// master) as an @c ISocket driven by @p loop's reactor. Ownership of the fd
/// transfers to the returned socket.
/// @param loop The loop whose reactor drives readiness (not owned).
/// @param fd The open, stream-capable descriptor.
/// @return The adopted socket; @c NetErrorCode::Unsupported on Windows.
[[nodiscard]] std::expected<std::unique_ptr<ISocket>, NetError> adoptFd(EventLoop& loop, int fd);

/// Adopts a connected stream socket that was accepted or dialled outside core-cpp, driven by
/// @p loop -- the loop the caller chooses, which need not be the one anything was accepted on.
///
/// It is how a Windows server spreads connections over several loops: Windows has no
/// `SO_REUSEPORT`, and a completion-port association is one socket to one port, so one thread
/// accepts and hands each raw handle to a loop in turn. It works the same on POSIX.
///
/// The socket built is the one @p loop's backend drives -- an I/O-completion socket where the loop
/// lends a completion port, a readiness socket otherwise -- which is the question
/// @c listen and @c connect ask of the loop too.
///
/// **Ownership of @p handle transfers on EVERY path, the error path included**: a handle that could
/// not be adopted is closed here before the error is returned, so the caller never closes it. That
/// is the opposite of @c adoptListener, and deliberately: a server dealing accepted connections out
/// has nothing useful to do with one a loop refused except close it, and making every caller write
/// that is making every caller able to forget it.
///
/// **It changes no socket option.** Not `TCP_NODELAY`, not the buffer sizes, not keepalive, not
/// inheritance: the socket was set up by whoever accepted it, and that caller owns those choices.
/// It sets only what the transport needs to work at all -- non-blocking mode on POSIX, and on
/// Windows whatever associating the socket with the loop's readiness event or completion port does.
///
/// Must be called on @p loop's thread, or before any thread drives it (asserted in Debug builds):
/// the socket joins the loop's registrations, which only that thread touches.
/// @param loop The loop to drive the socket (not owned; must outlive it).
/// @param handle The connected socket: a descriptor on POSIX, a `SOCKET` on Windows. Owned by this
///        call from the moment it is made.
/// @param peerAddress What @c ISocket::peerAddress reports; the caller knows it, this call does not
///        ask the kernel.
/// @return The adopted socket; @c NetErrorCode::BadHandle for an invalid handle, or why the loop
///         could not take it -- with @p handle closed either way.
[[nodiscard]] std::expected<std::unique_ptr<ISocket>, NetError> adoptSocket(EventLoop& loop,
                                                                            platform::NativeHandle handle,
                                                                            std::string peerAddress);

/// Appends one read chunk from @p socket to @p buffer — the accumulate step of
/// every binary-framed decode loop.
///
/// Reports EOF and failure DISTINCTLY: a decode loop that cannot tell "the peer hung up" from
/// "the transport broke" cannot say why it dropped a connection, which is the whole content of
/// the resulting diagnostic.
///
/// @param socket The transport to read from (not owned; a pointer, since
///        coroutine reference parameters can dangle).
/// @param buffer Receives the read bytes.
/// @return The number of bytes appended, 0 on a clean EOF, or the transport error.
[[nodiscard]] async::Task<IoResult> appendReadChunk(ISocket* socket, std::vector<std::byte>* buffer);

} // namespace core::net
