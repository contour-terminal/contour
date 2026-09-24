// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The handful of OS calls an outbound dial needs, behind one platform-free declaration.
///
/// **Two implementations chosen by the source list, not one body with an `#ifdef` in it**
/// (`.agent/rules/platform.md`): `posix/DialPrimitives.cpp` and `windows/DialPrimitives.cpp`. The
/// difference is real rather than cosmetic — on POSIX the thing the loop watches IS the socket,
/// while on Windows readiness for a socket arrives through a `WSAEVENT` associated with it — and
/// @c DialHandles is the shape that lets the dial itself stay written once.

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/KeepAlive.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace core::net
{

class EventLoop;

namespace detail
{

    /// The two handles one outstanding dial holds.
    ///
    /// They are the SAME value on POSIX and different on Windows, and that is the whole reason
    /// this struct exists: a dial that assumed one handle would have to be written twice.
    struct DialHandles
    {
        /// The connecting socket, as the platform spells one.
        platform::NativeHandle socket = platform::InvalidHandle;

        /// What the loop watches for this dial's readiness.
        platform::NativeHandle readiness = platform::InvalidHandle;

        /// What @c readiness is, for the backend that has to tell a socket from an event.
        HandleKind kind = DefaultHandleKind;

        /// @return True once @c openDialSocket has filled this in.
        [[nodiscard]] bool valid() const noexcept { return socket != platform::InvalidHandle; }
    };

    /// Whether a `::connect` answered at once or left the dial outstanding.
    enum class ConnectProgress : std::uint8_t
    {
        Completed, ///< The connection is established; no wait is needed. The loopback case.
        Pending,   ///< The kernel accepted the request; readiness will say how it ended.
    };

    /// Creates the non-blocking, close-on-exec socket a dial to @p endpoint needs, and whatever
    /// readiness object the platform requires alongside it.
    ///
    /// Close-on-exec matters here and not only for tidiness: a dialled socket is created by a
    /// plain `::socket` and so is NOT close-on-exec, unlike an accepted one — and a process that
    /// dials and also spawns children would otherwise hand every child an open peer connection.
    /// @param endpoint The candidate being dialled; its family and protocol choose the socket.
    /// @return The handles, or why none could be made.
    [[nodiscard]] std::expected<DialHandles, NetError> openDialSocket(ResolvedEndpoint const& endpoint);

    /// Closes what @c openDialSocket made, announcing the readiness handle to @p loop first.
    ///
    /// The announcement is unconditional even when nothing is parked, because the loop's contract
    /// is that a handle which MAY be registered is announced before it closes — epoll and kqueue
    /// can report neither a closed descriptor nor its former waiters.
    /// @param loop The loop the readiness handle may be registered with; may be null.
    /// @param handles What to close; left invalid.
    void closeDialSocket(EventLoop* loop, DialHandles& handles) noexcept;

    /// Issues the non-blocking `::connect`.
    /// @param handles What @c openDialSocket produced.
    /// @param endpoint The address to dial.
    /// @return Whether it completed or is outstanding, or why it failed outright.
    [[nodiscard]] std::expected<ConnectProgress, NetError> beginConnect(DialHandles const& handles,
                                                                        ResolvedEndpoint const& endpoint);

    /// Reads the socket's pending error — `getsockopt(SO_ERROR)` — which is the ONLY thing that
    /// says whether a dial resolved into a connection or into a refusal.
    ///
    /// **Ruling R101.** Readiness is not success: a refused connect also makes the socket ready,
    /// and which callback the kernel picks to say so is not portable. On Linux a failure can
    /// arrive with neither direction set; on macOS the write filter fires with `EV_EOF` and the
    /// backend reports `Writable`. A dial that believed the callback would hand its caller a
    /// socket whose first write fails.
    /// @param handles The dialling socket.
    /// @return Nothing when the connection is up, or the classified failure.
    [[nodiscard]] std::expected<void, NetError> pendingSocketError(DialHandles const& handles);

    /// Whether a dial on @p loop could hand out a socket at all, asked BEFORE anything is created:
    /// on Windows a loop whose backend lends no completion port cannot serve one, and a dial that
    /// found out at @c adoptDialled had already completed a real handshake the peer then saw
    /// reset. Everywhere else the answer is yes.
    /// @param loop The loop the dial would pin its socket to.
    /// @return Nothing, or @c NetErrorCode::Unsupported.
    [[nodiscard]] std::expected<void, NetError> dialableOn(EventLoop& loop);

    /// Wraps the connected handle as the platform's @c ISocket, transferring ownership.
    /// @param loop The loop the socket is pinned to.
    /// @param handles The connected handles; left invalid, because the socket owns them now.
    /// @param peer The printable peer address to report from @c ISocket::peerAddress.
    /// @return The socket, or why the loop cannot serve one -- on Windows, a loop with no
    ///         completion port. The handle is closed on failure.
    [[nodiscard]] SocketResult adoptDialled(EventLoop& loop, DialHandles& handles, std::string peer);

    /// Dials one candidate as ONE overlapped `ConnectEx` on @p loop's completion port — the
    /// completion-model counterpart of @c dialReadiness, and what a connector uses whenever
    /// @c EventLoop::completionPort answers.
    ///
    /// **Two things are the opposite of the readiness dial, and both are the completion model's**
    /// (the hand-off from Task B8 names them):
    ///
    /// - **The outcome is the completion's STATUS, never `SO_ERROR`.** Ruling R101's principle —
    ///   ask the operation, not the notification — carries over; its readiness idiom does not. A
    ///   successful completion is followed by `SO_UPDATE_CONNECT_CONTEXT`, without which the socket
    ///   refuses `getpeername` and `shutdown`.
    /// - **A deadline, or a stop, CANCELS the operation and lets the completion report.** The
    ///   completion is the single writer of the outcome; settling at the deadline instead would let
    ///   it land later into a frame that is gone. The abort then settles as @c NetErrorCode::Timeout
    ///   for the deadline and as `OperationCancelled` for a stop — and a connection the kernel had
    ///   already made before either took effect is handed back rather than thrown away.
    ///
    /// A platform whose loops never lend a completion port answers @c NetErrorCode::Unsupported,
    /// because no connector there asks.
    /// @param loop The loop whose port completes the dial; not owned.
    /// @param endpoint The candidate; by value, for the coroutine-frame reason.
    /// @param deadline When to give up on this candidate; `SteadyTimePoint::max()` for never.
    /// @param options The buffer sizes, asked for before the connect (@c applySocketBufferSizes),
    ///        and keepalive, armed after it (@c applyStreamSocketOptions).
    /// @return The connected socket, or why this candidate did not produce one.
    /// @throws async::OperationCancelled when the awaiting flow's own token stops the dial.
    [[nodiscard]] async::Task<SocketResult> dialCompletion(EventLoop* loop,
                                                           ResolvedEndpoint endpoint,
                                                           platform::SteadyTimePoint deadline,
                                                           StreamSocketOptions options);

} // namespace detail

} // namespace core::net
