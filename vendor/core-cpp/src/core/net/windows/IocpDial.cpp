// SPDX-License-Identifier: Apache-2.0

/// @file
/// `detail::dialCompletion` on Windows — one candidate dialled as an overlapped `ConnectEx` on the
/// loop's completion port, handing back an @c IocpSocket.
///
/// Upstream: fastcached `Net/IocpDial.hpp` and `Net/IocpConnector.cpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`. The per-dial op there lived in the dialling frame
/// and was safe BECAUSE the frame waited for its completion; here the op lives on the heap and
/// holds its own share until the port dequeues it, so a frame destroyed mid-dial -- a flow torn
/// down, a loop abandoning it -- is as safe as one that waited. It still waits, for the other
/// reason: the completion is the single writer of the outcome.

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
// clang-format on

#include <core/async/Cancellation.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/IocpOperation.hpp>
#include <core/net/windows/IocpSocket.hpp>
#include <core/net/windows/WinsockError.hpp>
#include <core/platform/WinsockInit.hpp>

#include <cstring>
#include <memory>
#include <tuple>
#include <utility>

namespace core::net::detail
{

namespace
{

    /// One outstanding `ConnectEx`. It holds itself while the kernel holds it.
    struct ConnectOperation: IocpOperation
    {
        std::shared_ptr<ConnectOperation> self;

        /// @param operation This node.
        static void released(IocpOperation& operation) noexcept
        {
            auto const keep = std::move(static_cast<ConnectOperation&>(operation).self);
        }
    };

    /// Binds @p socket to its family's wildcard address and an ephemeral port, which `ConnectEx`
    /// requires and `::connect` does implicitly.
    /// @param socket The unbound socket.
    /// @param family Its address family.
    /// @return Nothing, or why the bind failed.
    [[nodiscard]] std::expected<void, NetError> bindWildcard(SOCKET socket, int family)
    {
        auto local = sockaddr_storage {};
        auto length = 0;
        if (family == AF_INET6)
        {
            auto& v6 = reinterpret_cast<sockaddr_in6&>(local);
            v6.sin6_family = AF_INET6;
            v6.sin6_addr = in6addr_any;
            length = static_cast<int>(sizeof(sockaddr_in6));
        }
        else
        {
            auto& v4 = reinterpret_cast<sockaddr_in&>(local);
            v4.sin_family = AF_INET;
            v4.sin_addr.s_addr = INADDR_ANY;
            length = static_cast<int>(sizeof(sockaddr_in));
        }
        if (::bind(socket, reinterpret_cast<sockaddr const*>(&local), length) != 0)
            return std::unexpected(fromWinsockError(::WSAGetLastError(), "bind(ConnectEx)"));
        return {};
    }

    /// @param socket A socket of the provider whose `ConnectEx` is wanted.
    /// @return `ConnectEx`, or null.
    [[nodiscard]] LPFN_CONNECTEX connectExFor(SOCKET socket) noexcept
    {
        auto id = GUID WSAID_CONNECTEX;
        auto function = LPFN_CONNECTEX { nullptr };
        auto returned = DWORD { 0 };
        if (::WSAIoctl(socket,
                       SIO_GET_EXTENSION_FUNCTION_POINTER,
                       &id,
                       sizeof(id),
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

async::Task<SocketResult> dialCompletion(EventLoop* loop,
                                         ResolvedEndpoint endpoint,
                                         platform::SteadyTimePoint deadline,
                                         StreamSocketOptions options)
{
    platform::ensureWinsockInitialized();
    auto* const port = loop->completionPort();
    if (port == nullptr)
        co_return std::unexpected(
            makeNetError(NetErrorCode::Unsupported, 0, "the loop's backend lends no completion port"));

    auto socket = ::WSASocketW(endpoint.family,
                               SOCK_STREAM,
                               endpoint.protocol,
                               nullptr,
                               0,
                               WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == detail::InvalidSocket)
        co_return std::unexpected(fromWinsockError(::WSAGetLastError(), "socket"));

    // Closes whatever is LEFT when this frame goes -- an `OperationCancelled` thrown out of the wait
    // included. Closing is also what aborts a `ConnectEx` nothing will wait for; its completion
    // lands in the operation's own share. The success path empties `socket` instead of disarming.
    auto associated = false;
    auto const discard = ScopeGuard { [&]() noexcept {
        if (socket == detail::InvalidSocket)
            return;
        if (associated)
            port->forget(reinterpret_cast<platform::NativeHandle>(socket));
        ::closesocket(socket);
    } };

    // Before the connect, while the window scale can still take the receive buffer into account.
    applySocketBufferSizes(reinterpret_cast<platform::NativeHandle>(socket), options.buffers);
    if (auto bound = bindWildcard(socket, endpoint.family); !bound)
        co_return std::unexpected(std::move(bound.error()));
    auto* const connectEx = connectExFor(socket);
    if (connectEx == nullptr)
        co_return std::unexpected(fromWinsockError(::WSAGetLastError(), "WSAIoctl(ConnectEx)"));

    // BEFORE the operation: `ConnectEx` completes on whatever port the handle is associated with
    // when it is issued, which is why the socket is later told the association already exists.
    if (auto joined = port->associate(reinterpret_cast<platform::NativeHandle>(socket)); !joined)
        co_return std::unexpected(std::move(joined.error()));
    associated = true;

    auto operation = std::make_shared<ConnectOperation>();
    operation->onDequeued = &ConnectOperation::released;
    port->beginOperation(static_cast<IocpOperation*>(operation.get()));
    operation->self = operation;
    auto const* const address = reinterpret_cast<sockaddr const*>(endpoint.storage.data());
    if (connectEx(
            socket, address, static_cast<int>(endpoint.length), nullptr, 0, nullptr, &operation->overlapped)
        == FALSE)
    {
        auto const err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            port->withdrawOperation(static_cast<IocpOperation*>(operation.get()));
            operation->self.reset();
            co_return std::unexpected(fromWinsockError(err, "ConnectEx"));
        }
    }

    auto wait = CompletionWait { *loop, *operation, socket, deadline };
    co_await wait;

    switch (wait.outcome())
    {
        case CompletionWait::Outcome::Completed: break;
        case CompletionWait::Outcome::Refused:
            co_return std::unexpected(
                makeNetError(NetErrorCode::SystemError, 0, "the event loop refused to watch this dial"));
        case CompletionWait::Outcome::Abandoned:
        case CompletionWait::Outcome::Closed: // nothing closes a dial's socket from outside
        case CompletionWait::Outcome::Pending:
            // The loop is going away under the dial: unwind, as the readiness dial does. The guard
            // closes the socket, which aborts the connect into the operation's own share.
            throw async::OperationCancelled {};
    }

    // **The outcome is the completion's STATUS.** `SO_ERROR` is the readiness dial's question, and
    // it is not this one's: nothing here was told the socket became writable.
    if (auto const error = completionError(socket, *operation); error != 0)
    {
        if (isAbort(error) && wait.timedOut() && !wait.stopped())
            co_return std::unexpected(
                makeNetError(NetErrorCode::Timeout, static_cast<int>(error), "the connect deadline elapsed"));
        if (isAbort(error) && wait.stopped())
            // A cancel from the FLOW unwinds rather than reporting a value; the guard closes.
            throw async::OperationCancelled {};
        co_return std::unexpected(fromWinsockError(static_cast<int>(error), "ConnectEx"));
    }

    // Without this the socket is connected and yet `getpeername`, `shutdown` and the rest refuse
    // it: `ConnectEx` leaves the handle's context unset until asked.
    if (::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0)
        co_return std::unexpected(
            fromWinsockError(::WSAGetLastError(), "setsockopt(SO_UPDATE_CONNECT_CONTEXT)"));

    applyStreamSocketOptions(reinterpret_cast<platform::NativeHandle>(socket), options.keepAlive);

    auto const connected = std::exchange(socket, detail::InvalidSocket);
    co_return std::unique_ptr<ISocket> { new IocpSocket(
        *loop, connected, formatPeerAddress(endpoint), IocpAssociation::AlreadyAssociated) };
}

} // namespace core::net::detail
