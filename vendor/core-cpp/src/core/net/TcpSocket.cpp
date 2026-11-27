// SPDX-License-Identifier: Apache-2.0

/// @file
/// `openTcpSocket` and the one `ITcpSocket` implementation: the lifecycle, written once over the
/// platform's three primitives (`detail/TcpSocketPrimitives.hpp`) and the two dials.

#include <core/net/EventLoop.hpp>
#include <core/net/ITcpSocket.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/net/detail/SocketEndpoint.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>
#include <core/net/detail/TcpSocketPrimitives.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

namespace core::net
{

namespace
{
    /// @return The refusal of a socket that `connect` or `listen` consumed.
    [[nodiscard]] NetError consumed(char const* operation)
    {
        return makeNetError(NetErrorCode::BadHandle, 0, operation);
    }

    /// @return A task that answers @p error without touching any socket.
    async::Task<SocketResult> refuse(NetError error)
    {
        co_return std::unexpected(std::move(error));
    }

    /// @return @p family's wildcard address at port 0.
    [[nodiscard]] ResolvedEndpoint wildcardOf(AddressFamily family) noexcept
    {
        if (family == AddressFamily::Ipv6)
            return toEndpoint(IpSocketAddress { .address = Ipv6Address {} });
        return toEndpoint(IpSocketAddress { .address = Ipv4Address {} });
    }

    /// An unconnected TCP socket. It owns its handle until `connect` or a successful `listen` hands
    /// it on; from then on @c _socket is empty, which is the whole of the consumed state.
    class TcpSocket final: public ITcpSocket
    {
      public:
        /// @param loop The loop the socket is pinned to.
        /// @param family The family it was opened with: it picks the option rows and the wildcard.
        /// @param socket The open, unbound socket.
        TcpSocket(EventLoop& loop, AddressFamily family, detail::DialSocket socket):
            _loop { loop }, _family { family }, _socket { std::in_place, std::move(socket) }
        {
        }

        [[nodiscard]] std::expected<void, NetError> bind(ResolvedEndpoint const& local) override
        {
            if (!_socket.has_value())
                return std::unexpected(consumed("bind on a consumed TCP socket"));
            // Refused here rather than by the kernel, whose second bind is an `EINVAL` that reads
            // like a bad address.
            if (_binding == detail::LocalBinding::Bound)
                return std::unexpected(consumed("bind on a TCP socket that is already bound"));
            auto const handle = _socket->handles().socket;
            for (auto const option: detail::reuseAddressOptions())
                if (auto set = detail::setIntOption(handle, option, 1); !set.has_value())
                    return std::unexpected(std::move(set.error()));
            return detail::bindTcpHandle(handle, local).transform([this] {
                _binding = detail::LocalBinding::Bound;
            });
        }

        [[nodiscard]] async::Task<SocketResult> connect(ResolvedEndpoint remote) override
        {
            if (!_socket.has_value())
                return refuse(consumed("connect on a consumed TCP socket"));

            // Taken NOW, not when the task first runs: the task owns the socket from here, and closes
            // it if it is dropped unstarted.
            auto socket = std::move(*_socket);
            _socket.reset();
            auto const max = platform::SteadyTimePoint::max();

            // The loop decides, as it does for a connector: a completion port where it lends one
            // (Windows, where every loop that can serve a socket does), readiness everywhere else.
            if (_loop.completionPort() != nullptr)
                return detail::dialCompletionOn(
                    &_loop, std::move(socket), _binding, remote, max, detail::StreamSocketOptions {});
            return detail::dialReadinessOn(
                &_loop, std::move(socket), remote, max, detail::StreamSocketOptions {});
        }

        [[nodiscard]] std::expected<std::unique_ptr<IListener>, NetError> listen(
            std::uint32_t backlog) override
        {
            if (!_socket.has_value())
                return std::unexpected(consumed("listen on a consumed TCP socket"));

            // Explicitly, on every platform: Windows refuses to listen on an unbound socket.
            if (_binding == detail::LocalBinding::Unbound)
                if (auto bound = bind(wildcardOf(_family)); !bound.has_value())
                    return std::unexpected(std::move(bound.error()));

            auto const handle = _socket->handles().socket;
            auto const clamped = static_cast<int>(std::min<std::uint32_t>(backlog, INT_MAX));
            if (auto listening = detail::listenTcpHandle(handle, clamped); !listening.has_value())
                return std::unexpected(std::move(listening.error()));

            // Listening from here, so it can never connect: a failure to adopt it consumes it too,
            // rather than leaving a socket whose next `connect` the kernel refuses.
            auto adopted = adoptListener(_loop, handle);
            if (adopted.has_value())
                std::ignore = _socket->release(); // the listener closes the handle now
            _socket.reset();
            return adopted;
        }

        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> localEndpoint() const override
        {
            if (!_socket.has_value())
                return std::unexpected(consumed("localEndpoint on a consumed TCP socket"));
            // Not asked of the OS: an unbound socket is the wildcard at port 0 on POSIX and an error
            // on Windows, and one answer on every platform is the point.
            if (_binding == detail::LocalBinding::Unbound)
                return std::unexpected(consumed("localEndpoint on an unbound TCP socket"));
            return detail::socketEndpoint(_socket->handles().socket, detail::SocketSide::Local);
        }

        [[nodiscard]] std::expected<std::uint64_t, NetError> option(SocketOption option) const override
        {
            if (!_socket.has_value())
                return std::unexpected(consumed("option on a consumed TCP socket"));
            return detail::readSocketOption(_socket->handles().socket, _family, option);
        }

        [[nodiscard]] std::expected<void, NetError> setOption(SocketOption option,
                                                              std::uint64_t value) override
        {
            if (!_socket.has_value())
                return std::unexpected(consumed("setOption on a consumed TCP socket"));
            return detail::writeSocketOption(_socket->handles().socket, _family, option, value);
        }

      private:
        EventLoop& _loop;
        AddressFamily _family;
        std::optional<detail::DialSocket> _socket;
        detail::LocalBinding _binding = detail::LocalBinding::Unbound;
    };
} // namespace

std::expected<std::unique_ptr<ITcpSocket>, NetError> openTcpSocket(EventLoop& loop, AddressFamily family)
{
    // Before the socket exists, as a dial asks: a loop that could not adopt the connection or the
    // listener this socket becomes refuses it now, not at the connect.
    return detail::dialableOn(loop)
        .and_then([family] { return detail::openTcpHandles(family); })
        .transform([&loop, family](detail::DialHandles handles) -> std::unique_ptr<ITcpSocket> {
            return std::make_unique<TcpSocket>(loop, family, detail::DialSocket { &loop, handles });
        });
}

} // namespace core::net
