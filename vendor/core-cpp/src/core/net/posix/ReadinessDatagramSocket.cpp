// SPDX-License-Identifier: Apache-2.0

/// @file
/// The POSIX `detail::IDatagramChannel`: a non-blocking UDP socket that parks on the loop's
/// readiness (`EventLoop::waitReadable`/`waitWritable`) and then calls `recvmsg`/`sendto`.

#include <core/net/EventLoop.hpp>
#include <core/net/SocketContract.hpp>
#include <core/net/detail/DatagramChannel.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>
#include <core/net/detail/WouldBlock.hpp>
#include <core/net/posix/FdUtils.hpp>

#include <sys/socket.h>
#include <sys/uio.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <unistd.h>

#include <netinet/in.h>

namespace core::net::detail
{

namespace
{
    /// What `connect(AF_UNSPEC)` may answer although it did disconnect the socket. macOS documents
    /// `EAFNOSUPPORT` as "harmlessly returned" and answers `EINVAL` for some families; Linux and the
    /// other BSDs answer 0, so on them this row is never reached.
    constexpr auto HarmlessDisconnectErrors = std::array { EAFNOSUPPORT, EINVAL };

    /// A UDP socket driven by readiness.
    ///
    /// The descriptor is -1 once closed, which a parked flow reads on its way out: the close
    /// announces itself to the loop (`notifyHandleClosing` with `FdWakePolicy::Resume`), so the
    /// flow resumes on its normal path and finds the channel closed. That is safe because the flow
    /// holds its own share of the channel, whoever destroyed the socket.
    class ReadinessDatagramChannel final: public IDatagramChannel
    {
      public:
        /// @param loop The loop to park on.
        /// @param fd The open socket; owned from here.
        /// @param family The family it was opened with, which picks the option rows.
        ReadinessDatagramChannel(EventLoop& loop, int fd, AddressFamily family) noexcept:
            _loop { loop }, _fd { fd }, _family { family }
        {
        }

        ~ReadinessDatagramChannel() override { close(); }
        ReadinessDatagramChannel(ReadinessDatagramChannel const&) = delete;
        ReadinessDatagramChannel& operator=(ReadinessDatagramChannel const&) = delete;
        ReadinessDatagramChannel(ReadinessDatagramChannel&&) = delete;
        ReadinessDatagramChannel& operator=(ReadinessDatagramChannel&&) = delete;

        [[nodiscard]] std::expected<void, NetError> bind(ResolvedEndpoint const& local) override
        {
            if (::bind(_fd, addressOf(local), static_cast<socklen_t>(local.length)) != 0)
                return std::unexpected(socketError(errno, "bind"));
            return {};
        }

        [[nodiscard]] std::expected<void, NetError> connect(
            std::optional<ResolvedEndpoint> const& remote) override
        {
            if (remote.has_value())
            {
                if (::connect(_fd, addressOf(*remote), static_cast<socklen_t>(remote->length)) != 0)
                    return std::unexpected(socketError(errno, "connect"));
                return {};
            }
            auto unspecified = sockaddr_storage {};
            unspecified.ss_family = AF_UNSPEC;
            if (::connect(_fd, reinterpret_cast<sockaddr const*>(&unspecified), sizeof(sockaddr)) != 0)
                if (auto const error = errno;
                    std::ranges::find(HarmlessDisconnectErrors, error) == HarmlessDisconnectErrors.end())
                    return std::unexpected(socketError(error, "connect(AF_UNSPEC)"));
            return {};
        }

        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> endpoint(SocketSide side) const override
        {
            return socketEndpoint(_fd, side);
        }

        [[nodiscard]] std::expected<std::uint64_t, NetError> option(SocketOption option) const override
        {
            return readSocketOption(_fd, _family, option);
        }

        [[nodiscard]] std::expected<void, NetError> setOption(SocketOption option,
                                                              std::uint64_t value) override
        {
            return writeSocketOption(_fd, _family, option, value);
        }

        [[nodiscard]] async::Task<std::expected<AsyncDatagram, NetError>> receive(
            std::size_t maxBytes) override
        {
            return receiveOn(std::static_pointer_cast<ReadinessDatagramChannel>(shared_from_this()),
                             maxBytes);
        }

        [[nodiscard]] async::Task<std::expected<void, NetError>> send(
            std::vector<std::byte> payload, std::optional<ResolvedEndpoint> to) override
        {
            return sendOn(std::static_pointer_cast<ReadinessDatagramChannel>(shared_from_this()),
                          std::move(payload),
                          to);
        }

        void close() noexcept override
        {
            if (_fd < 0)
                return;
            contract::assertTeardownIsSerialisedWithDispatch(_loop);
            // Before the close, while the descriptor still names this socket: epoll and kqueue
            // forget a closed descriptor silently, and a parked flow would never resume.
            _loop.notifyHandleClosing(_fd, FdWakePolicy::Resume);
            ::close(std::exchange(_fd, -1));
        }

      private:
        /// @return @p endpoint's sockaddr.
        [[nodiscard]] static sockaddr const* addressOf(ResolvedEndpoint const& endpoint) noexcept
        {
            return reinterpret_cast<sockaddr const*>(endpoint.storage.data());
        }

        /// The receive: one `recvmsg` whenever the socket is readable, until one answers.
        ///
        /// `recvmsg` rather than `recvfrom` for `MSG_TRUNC` in `msg_flags`, which every POSIX
        /// kernel sets when the datagram was longer than the buffer -- and has then dropped the
        /// rest, so the datagram is answered as too large rather than handed back cut short.
        static async::Task<std::expected<AsyncDatagram, NetError>> receiveOn(
            std::shared_ptr<ReadinessDatagramChannel> self, std::size_t maxBytes)
        {
            auto payload = std::vector<std::byte>(maxBytes);
            while (true)
            {
                if (self->_fd < 0)
                    co_return std::unexpected(closedUnder("receive"));
                auto from = sockaddr_storage {};
                auto vector = iovec { .iov_base = payload.data(), .iov_len = payload.size() };
                auto message = msghdr {};
                message.msg_name = &from;
                message.msg_namelen = sizeof(from);
                message.msg_iov = &vector;
                message.msg_iovlen = 1;
                auto const received = ::recvmsg(self->_fd, &message, 0);
                if (received >= 0)
                {
                    if ((message.msg_flags & MSG_TRUNC) != 0)
                        co_return std::unexpected(
                            makeNetError(NetErrorCode::MessageTooLarge,
                                         0,
                                         "receive: the datagram is longer than maxBytes"));
                    payload.resize(static_cast<std::size_t>(received));
                    co_return AsyncDatagram { .payload = std::move(payload),
                                              .from = endpointFromSockaddr(&from, message.msg_namelen) };
                }
                if (auto const error = errno; !detail::isWouldBlock(error))
                    co_return std::unexpected(socketError(error, "recvmsg"));
                // The stop path: a cancelled flow throws out of this wait. Nothing was taken from
                // the kernel yet, so a datagram that arrived meanwhile is the next receive's.
                co_await self->_loop.waitReadable(self->_fd);
            }
        }

        /// The send: one `sendto` whenever the socket is writable, until one answers.
        static async::Task<std::expected<void, NetError>> sendOn(
            std::shared_ptr<ReadinessDatagramChannel> self,
            std::vector<std::byte> payload,
            std::optional<ResolvedEndpoint> to)
        {
            auto const* const address = to.has_value() ? addressOf(*to) : nullptr;
            auto const length = to.has_value() ? static_cast<socklen_t>(to->length) : socklen_t { 0 };
            while (true)
            {
                if (self->_fd < 0)
                    co_return std::unexpected(closedUnder("send"));
                auto const sent =
                    ::sendto(self->_fd, payload.data(), payload.size(), MSG_NOSIGNAL, address, length);
                if (sent >= 0)
                {
                    // Not reachable on a POSIX kernel, which sends a datagram whole or fails it with
                    // EMSGSIZE, so no test can reach it; kept because the rule is that a partial
                    // datagram send is never retried -- the rest resent would be a second message.
                    if (static_cast<std::size_t>(sent) != payload.size())
                        co_return std::unexpected(makeNetError(
                            NetErrorCode::MessageTooLarge, 0, "sendto placed part of the datagram"));
                    co_return {};
                }
                if (auto const error = errno; !detail::isWouldBlock(error))
                    co_return std::unexpected(socketError(error, "sendto"));
                co_await self->_loop.waitWritable(self->_fd);
            }
        }

        EventLoop& _loop;
        int _fd;
        AddressFamily _family;
    };
} // namespace

std::expected<std::shared_ptr<IDatagramChannel>, NetError> openDatagramChannel(EventLoop& loop,
                                                                               AddressFamily family)
{
    auto const fd = makeDatagramSocket(family == AddressFamily::Ipv6 ? AF_INET6 : AF_INET, 0);
    if (fd < 0)
        return std::unexpected(socketError(errno, "socket"));
    // From here the channel owns the descriptor, and closes it on every way out.
    auto channel = std::make_shared<ReadinessDatagramChannel>(loop, fd, family);
    if (!makeNonBlockingCloexec(fd))
        return std::unexpected(socketError(errno, "fcntl(O_NONBLOCK)"));
    if (family == AddressFamily::Ipv6)
        if (auto only = setIntOption(fd, NativeOption { .level = IPPROTO_IPV6, .name = IPV6_V6ONLY }, 1);
            !only.has_value())
            return std::unexpected(std::move(only.error()));
    return channel;
}

} // namespace core::net::detail
