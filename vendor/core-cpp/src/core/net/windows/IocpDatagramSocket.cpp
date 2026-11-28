// SPDX-License-Identifier: Apache-2.0

/// @file
/// The Windows `detail::IDatagramChannel`: an overlapped UDP socket on the loop's completion port,
/// whose receives and sends are `WSARecvFrom` and `WSASendTo` waited for through
/// `detail::CompletionWait`.

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
// clang-format on

#include <core/async/Cancellation.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ICompletionPort.hpp>
#include <core/net/SocketContract.hpp>
#include <core/net/detail/DatagramChannel.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/IocpOperation.hpp>
#include <core/net/windows/WinsockError.hpp>
#include <core/platform/WinsockInit.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace core::net::detail
{

namespace
{
    /// One outstanding `WSARecvFrom` or `WSASendTo`, and everything the kernel writes into or reads
    /// from until it completes: the payload, and for a receive the sender's address and its length.
    ///
    /// **It holds itself while the kernel holds it** (@c self), and the port gives that share back
    /// when the packet is dequeued, so neither the channel nor the waiting frame has to outlive the
    /// kernel's use of it ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)).
    struct DatagramOperation: IocpOperation
    {
        std::vector<std::byte> payload;                              ///< Received into, or sent from.
        sockaddr_storage from {};                                    ///< A receive's sender.
        INT fromLength = static_cast<INT>(sizeof(sockaddr_storage)); ///< Its length, in and out.
        DWORD flags = 0;                                             ///< A receive's flags, in and out.
        std::optional<ResolvedEndpoint> to;                          ///< A send's destination.
        std::shared_ptr<DatagramOperation> self;

        /// @param operation This operation.
        static void released(IocpOperation& operation) noexcept
        {
            auto const keep = std::move(static_cast<DatagramOperation&>(operation).self);
        }
    };

    /// @return Whether @p error, from an issued `WSARecvFrom`, is a datagram longer than the buffer.
    ///
    /// Answered at once as @c NetErrorCode::MessageTooLarge, never waited for: the kernel's status
    /// is the warning `STATUS_BUFFER_OVERFLOW`, for which the I/O manager may or may not still queue
    /// a packet, and a wait for one that never comes would park the receive for ever. The operation
    /// is therefore left announced and holding itself, rather than withdrawn: a late packet is then
    /// recognised and frees it through `onDequeued`, and none costs only the port's drain diagnostic.
    [[nodiscard]] constexpr bool isTruncation(int error) noexcept
    {
        return error == WSAEMSGSIZE || error == ERROR_MORE_DATA;
    }

    /// A UDP socket on a completion port.
    class IocpDatagramChannel final: public IDatagramChannel
    {
      public:
        /// @param loop The loop whose port completes this socket's operations.
        /// @param socket The open overlapped socket; owned from here.
        /// @param family The family it was opened with.
        IocpDatagramChannel(EventLoop& loop, SOCKET socket, AddressFamily family) noexcept:
            _loop { loop }, _port { loop.completionPort() }, _socket { socket }, _family { family }
        {
        }

        ~IocpDatagramChannel() override { close(); }
        IocpDatagramChannel(IocpDatagramChannel const&) = delete;
        IocpDatagramChannel& operator=(IocpDatagramChannel const&) = delete;
        IocpDatagramChannel(IocpDatagramChannel&&) = delete;
        IocpDatagramChannel& operator=(IocpDatagramChannel&&) = delete;

        /// Associates the socket with the loop's port and turns off what an unconnected UDP socket
        /// must not do on Windows: fail its next receive with `WSAECONNRESET` because an earlier
        /// send drew an ICMP port-unreachable.
        /// @return Nothing, or why the socket cannot be served.
        [[nodiscard]] std::expected<void, NetError> prepare()
        {
            if (_family == AddressFamily::Ipv6)
                if (auto only = setIntOption(
                        handle(), NativeOption { .level = IPPROTO_IPV6, .name = IPV6_V6ONLY }, 1);
                    !only.has_value())
                    return std::unexpected(std::move(only.error()));
            auto reportReset = BOOL { FALSE };
            auto returned = DWORD { 0 };
            if (::WSAIoctl(_socket,
                           SIO_UDP_CONNRESET,
                           &reportReset,
                           sizeof(reportReset),
                           nullptr,
                           0,
                           &returned,
                           nullptr,
                           nullptr)
                == SOCKET_ERROR)
                return std::unexpected(fromWinsockError(::WSAGetLastError(), "WSAIoctl(SIO_UDP_CONNRESET)"));
            return _port->associate(handle()).transform([this] { _isAssociated = true; });
        }

        [[nodiscard]] std::expected<void, NetError> bind(ResolvedEndpoint const& local) override
        {
            if (::bind(_socket, addressOf(local), static_cast<int>(local.length)) != 0)
                return std::unexpected(fromWinsockError(::WSAGetLastError(), "bind"));
            return {};
        }

        [[nodiscard]] std::expected<void, NetError> connect(
            std::optional<ResolvedEndpoint> const& remote) override
        {
            if (remote.has_value())
                return connectTo(*remote);
            // An all-zero address of the socket's family is Winsock's documented disconnect. Not
            // `AF_UNSPEC`, which libuv sends: wine hands that to Linux, which then takes the
            // kernel-chosen port away while wine still refuses the bind that would restore it.
            auto zero = ResolvedEndpoint {};
            zero.length = static_cast<std::uint32_t>(_family == AddressFamily::Ipv6 ? sizeof(sockaddr_in6)
                                                                                    : sizeof(sockaddr_in));
            reinterpret_cast<sockaddr*>(zero.storage.data())->sa_family =
                static_cast<ADDRESS_FAMILY>(_family == AddressFamily::Ipv6 ? AF_INET6 : AF_INET);
            return connectTo(zero);
        }

        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> endpoint(SocketSide side) const override
        {
            return socketEndpoint(handle(), side);
        }

        [[nodiscard]] std::expected<std::uint64_t, NetError> option(SocketOption option) const override
        {
            return readSocketOption(handle(), _family, option);
        }

        [[nodiscard]] std::expected<void, NetError> setOption(SocketOption option,
                                                              std::uint64_t value) override
        {
            return writeSocketOption(handle(), _family, option, value);
        }

        [[nodiscard]] async::Task<std::expected<AsyncDatagram, NetError>> receive(
            std::size_t maxBytes) override
        {
            return receiveOn(std::static_pointer_cast<IocpDatagramChannel>(shared_from_this()), maxBytes);
        }

        [[nodiscard]] async::Task<std::expected<void, NetError>> send(
            std::vector<std::byte> payload, std::optional<ResolvedEndpoint> to) override
        {
            return sendOn(
                std::static_pointer_cast<IocpDatagramChannel>(shared_from_this()), std::move(payload), to);
        }

        void close() noexcept override
        {
            if (_socket == InvalidSocket)
                return;
            contract::assertTeardownIsSerialisedWithDispatch(_loop);
            // Taken first: a wait resolved below may resume a flow that ends this channel's life.
            auto const waits = std::exchange(_waits, {});
            if (_isAssociated)
                _port->forget(handle());
            // What aborts every operation still outstanding; each completes later into its own share.
            ::closesocket(std::exchange(_socket, InvalidSocket));
            for (auto* const wait: waits)
                wait->close();
        }

      private:
        /// @return The socket as the platform's handle.
        [[nodiscard]] platform::NativeHandle handle() const noexcept
        {
            return reinterpret_cast<platform::NativeHandle>(_socket);
        }

        /// @return @p endpoint's sockaddr.
        [[nodiscard]] static sockaddr const* addressOf(ResolvedEndpoint const& endpoint) noexcept
        {
            return reinterpret_cast<sockaddr const*>(endpoint.storage.data());
        }

        /// @return Nothing, or why `connect` to @p remote was refused.
        [[nodiscard]] std::expected<void, NetError> connectTo(ResolvedEndpoint const& remote) const
        {
            if (::connect(_socket, addressOf(remote), static_cast<int>(remote.length)) != 0)
                return std::unexpected(fromWinsockError(::WSAGetLastError(), "connect"));
            return {};
        }

        /// @return A fresh operation over @p payload, announced to the port and holding itself.
        [[nodiscard]] std::shared_ptr<DatagramOperation> begin(std::vector<std::byte> payload)
        {
            auto operation = std::make_shared<DatagramOperation>();
            operation->payload = std::move(payload);
            operation->onDequeued = &DatagramOperation::released;
            // Announced BEFORE the call: the packet can be queued the instant it is issued.
            _port->beginOperation(static_cast<IocpOperation*>(operation.get()));
            operation->self = operation;
            return operation;
        }

        /// Takes back an operation the kernel refused outright, which no packet will name.
        void withdraw(DatagramOperation& operation) noexcept
        {
            _port->withdrawOperation(static_cast<IocpOperation*>(&operation));
            operation.self.reset();
        }

        /// Waits for @p operation's completion and says what it did.
        ///
        /// The completion is the single writer of the outcome: a stop asks the kernel for the
        /// operation back and goes on waiting, so a receive that completed with a datagram first is
        /// still returned. Only an abort the stop caused unwinds the flow.
        /// @return The bytes transferred.
        /// @throws async::OperationCancelled where the flow's stop took the operation back.
        static async::Task<std::expected<std::size_t, NetError>> completionOf(
            std::shared_ptr<IocpDatagramChannel> self,
            std::shared_ptr<DatagramOperation> operation,
            char const* what)
        {
            auto wait = CompletionWait { self->_loop, *operation, self->_socket };
            self->_waits.push_back(&wait);
            // Delisted however the frame leaves, so a later close never reaches a wait that is gone.
            auto const listed = ScopeGuard { [&self, &wait]() noexcept { std::erase(self->_waits, &wait); } };
            co_await wait;

            switch (wait.outcome())
            {
                case CompletionWait::Outcome::Completed: break;
                case CompletionWait::Outcome::Closed: co_return std::unexpected(closedUnder(what));
                case CompletionWait::Outcome::Refused:
                    co_return std::unexpected(makeNetError(
                        NetErrorCode::SystemError, 0, std::string { what } + ": the loop refused the park"));
                case CompletionWait::Outcome::Pending:
                case CompletionWait::Outcome::Abandoned: throw async::OperationCancelled {};
            }
            auto const error = completionError(self->_socket, *operation);
            if (error == 0)
                co_return operation->bytesTransferred();
            if (isAbort(error))
            {
                if (wait.stopped())
                    throw async::OperationCancelled {};
                co_return std::unexpected(closedUnder(what));
            }
            co_return std::unexpected(fromWinsockError(static_cast<int>(error), what));
        }

        static async::Task<std::expected<AsyncDatagram, NetError>> receiveOn(
            std::shared_ptr<IocpDatagramChannel> self, std::size_t maxBytes)
        {
            if (self->_socket == InvalidSocket)
                co_return std::unexpected(closedUnder("receive"));
            auto const operation = self->begin(std::vector<std::byte>(maxBytes));
            auto buffer = WSABUF { .len = static_cast<ULONG>(operation->payload.size()),
                                   .buf = reinterpret_cast<CHAR*>(operation->payload.data()) };
            auto error = 0;
            if (::WSARecvFrom(self->_socket,
                              &buffer,
                              1,
                              nullptr,
                              &operation->flags,
                              reinterpret_cast<sockaddr*>(&operation->from),
                              &operation->fromLength,
                              reinterpret_cast<LPWSAOVERLAPPED>(&operation->overlapped),
                              nullptr)
                == SOCKET_ERROR)
                error = ::WSAGetLastError();
            if (isTruncation(error))
                co_return std::unexpected(makeNetError(NetErrorCode::MessageTooLarge,
                                                       error,
                                                       "WSARecvFrom: the datagram is longer than maxBytes"));
            if (error != 0 && error != WSA_IO_PENDING)
            {
                self->withdraw(*operation);
                co_return std::unexpected(fromWinsockError(error, "WSARecvFrom"));
            }

            auto transferred = co_await completionOf(self, operation, "WSARecvFrom");
            if (!transferred.has_value())
                co_return std::unexpected(std::move(transferred.error()));
            operation->payload.resize(*transferred);
            co_return AsyncDatagram {
                .payload = std::move(operation->payload),
                .from =
                    endpointFromSockaddr(&operation->from, static_cast<std::uint32_t>(operation->fromLength)),
            };
        }

        static async::Task<std::expected<void, NetError>> sendOn(std::shared_ptr<IocpDatagramChannel> self,
                                                                 std::vector<std::byte> payload,
                                                                 std::optional<ResolvedEndpoint> to)
        {
            if (self->_socket == InvalidSocket)
                co_return std::unexpected(closedUnder("send"));
            auto const size = payload.size();
            auto const operation = self->begin(std::move(payload));
            operation->to = to;
            auto buffer = WSABUF { .len = static_cast<ULONG>(operation->payload.size()),
                                   .buf = reinterpret_cast<CHAR*>(operation->payload.data()) };
            auto error = 0;
            if (::WSASendTo(self->_socket,
                            &buffer,
                            1,
                            nullptr,
                            0,
                            operation->to.has_value() ? addressOf(*operation->to) : nullptr,
                            operation->to.has_value() ? static_cast<int>(operation->to->length) : 0,
                            reinterpret_cast<LPWSAOVERLAPPED>(&operation->overlapped),
                            nullptr)
                == SOCKET_ERROR)
                error = ::WSAGetLastError();
            if (error != 0 && error != WSA_IO_PENDING)
            {
                self->withdraw(*operation);
                co_return std::unexpected(fromWinsockError(error, "WSASendTo"));
            }

            auto transferred = co_await completionOf(self, operation, "WSASendTo");
            if (!transferred.has_value())
                co_return std::unexpected(std::move(transferred.error()));
            // Not reachable through Winsock, which places a datagram whole or fails it with
            // WSAEMSGSIZE; kept because the rule is that a partial datagram send is never retried.
            if (*transferred != size)
                co_return std::unexpected(
                    makeNetError(NetErrorCode::MessageTooLarge, 0, "WSASendTo placed part of the datagram"));
            co_return {};
        }

        EventLoop& _loop;
        ICompletionPort* _port;
        SOCKET _socket;
        AddressFamily _family;
        bool _isAssociated = false;
        std::vector<CompletionWait*> _waits; ///< Every operation parked right now, for @c close.
    };
} // namespace

std::expected<std::shared_ptr<IDatagramChannel>, NetError> openDatagramChannel(EventLoop& loop,
                                                                               AddressFamily family)
{
    platform::ensureWinsockInitialized();
    auto const socket = ::WSASocketW(family == AddressFamily::Ipv6 ? AF_INET6 : AF_INET,
                                     SOCK_DGRAM,
                                     IPPROTO_UDP,
                                     nullptr,
                                     0,
                                     WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == InvalidSocket)
        return std::unexpected(fromWinsockError(::WSAGetLastError(), "socket"));
    // From here the channel owns the socket, and closes it on every way out.
    auto channel = std::make_shared<IocpDatagramChannel>(loop, socket, family);
    return channel->prepare().transform(
        [&] { return std::shared_ptr<IDatagramChannel> { std::move(channel) }; });
}

} // namespace core::net::detail
