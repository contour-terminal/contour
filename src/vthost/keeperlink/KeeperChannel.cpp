// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/KeeperChannel.hpp>

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <ranges>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <vthost/keeper/Codec.hpp>
#include <vthost/keeper/DescriptorSocket.hpp>

namespace vthost::keeperlink
{

namespace
{
    /// How much one read takes at most.
    constexpr auto ReadChunk = std::size_t { 64 } * 1024;

    /// @return The milliseconds poll() may wait until @p deadline, at least 0.
    [[nodiscard]] int pollTimeout(core::platform::IClock const& clock,
                                  core::platform::SteadyTimePoint deadline)
    {
        auto const left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock.now());
        return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count(), 0, 60'000));
    }

    /// Whether a wait saw what it waited for.
    enum class Readiness : uint8_t
    {
        Ready = 0, ///< The descriptor reported the events, or an error the call that follows reports.
        TimedOut,  ///< The deadline passed first.
    };

    /// Waits until @p fd reports @p events or @p deadline passes.
    /// @return Whether it did before the deadline.
    [[nodiscard]] Readiness awaitEvents(int fd,
                                        short events,
                                        core::platform::IClock const& clock,
                                        core::platform::SteadyTimePoint deadline)
    {
        while (true)
        {
            auto poller = ::pollfd { .fd = fd, .events = events, .revents = 0 };
            auto const ready = ::poll(&poller, 1, pollTimeout(clock, deadline));
            if (ready > 0)
                return Readiness::Ready;
            if (ready < 0 && errno != EINTR)
                return Readiness::Ready; // let the read or write that follows report it
            if (ready == 0 && clock.now() >= deadline)
                return Readiness::TimedOut;
        }
    }
} // namespace

std::vector<uint8_t> frameBytesOf(keeper::Message const& message, std::size_t fdCount)
{
    // Descriptors travel beside the bytes; encodeFrame() reads only how many there are.
    auto const placeholders = std::vector<int>(fdCount, -1);
    return keeper::encodeFrame(keeper::Frame { .body = keeper::encode(message), .fds = placeholders });
}

KeeperChannel::KeeperChannel(keeper::UniqueFd socket): _socket { std::move(socket) }, _readBuffer(ReadChunk)
{
    if (auto const flags = ::fcntl(_socket.get(), F_GETFL); flags != -1)
        ::fcntl(_socket.get(), F_SETFL, flags | O_NONBLOCK);
    // No child inherits it: a live upgrade's exec keeps it across explicitly (LiveUpgradeConfig::crossing).
    ::fcntl(_socket.get(), F_SETFD, FD_CLOEXEC);
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    auto const on = 1;
    ::setsockopt(_socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

std::expected<std::optional<std::size_t>, ChannelError> KeeperChannel::readOnce()
{
    auto received = keeper::receiveWithDescriptors(_socket.get(), _readBuffer);
    if (!received)
    {
        switch (received.error())
        {
            case keeper::ReceiveError::WouldBlock: return std::nullopt;
            case keeper::ReceiveError::PeerGone: return std::unexpected { ChannelError::Closed };
            case keeper::ReceiveError::Broken:
            case keeper::ReceiveError::Truncated: break;
        }
        return std::unexpected { ChannelError::Broken };
    }
    _assembler.feed(std::span { _readBuffer }.first(received->bytes), received->fds);
    return received->bytes;
}

std::expected<void, ChannelError> KeeperChannel::collect()
{
    while (auto frame = _assembler.next())
    {
        auto fds = std::vector<keeper::UniqueFd> {};
        for (auto const fd: frame->fds)
            fds.emplace_back(fd);
        auto decoded = keeper::decodeFrame(*frame);
        if (decoded)
        {
            _ready.push_back(ReceivedMessage { .message = std::move(*decoded), .fds = std::move(fds) });
            continue;
        }
        auto const tag = keeper::peekTag(frame->body);
        if (!tag || keeper::failureOf(decoded.error()) != keeper::DecodeFailure::Unsupported)
            return std::unexpected { ChannelError::Broken };
        _ready.push_back(ReceivedMessage { .message = std::unexpected { UnreadableMessage { .tag = *tag } },
                                           .fds = std::move(fds) });
    }
    if (_assembler.error())
    {
        keeper::closeAll(_assembler.takePendingFds());
        return std::unexpected { ChannelError::Broken };
    }
    return {};
}

std::expected<std::vector<ReceivedMessage>, ChannelError> KeeperChannel::readAvailable()
{
    auto closed = false;
    while (true)
    {
        auto const read = readOnce();
        if (!read)
            return std::unexpected { read.error() };
        if (!*read)
            break; // nothing more for now
        if (**read == 0)
        {
            closed = true;
            break;
        }
    }
    if (auto const collected = collect(); !collected)
        return std::unexpected { collected.error() };
    auto messages = takeReady();
    // What arrived before the end is handed out first; the end is reported by the next call.
    if (closed && messages.empty())
    {
        keeper::closeAll(_assembler.takePendingFds());
        return std::unexpected { ChannelError::Closed };
    }
    return messages;
}

std::expected<ReceivedMessage, ChannelError> KeeperChannel::receive(core::platform::IClock const& clock,
                                                                    core::platform::SteadyTimePoint deadline)
{
    while (_ready.empty())
    {
        if (awaitEvents(_socket.get(), POLLIN, clock, deadline) == Readiness::TimedOut)
            return std::unexpected { ChannelError::TimedOut };
        auto const read = readOnce();
        if (!read)
            return std::unexpected { read.error() };
        if (!*read)
            continue;
        if (auto const collected = collect(); !collected)
            return std::unexpected { collected.error() };
        if (**read == 0 && _ready.empty())
        {
            keeper::closeAll(_assembler.takePendingFds());
            return std::unexpected { ChannelError::Closed };
        }
    }
    auto message = std::move(_ready.front());
    _ready.pop_front();
    return message;
}

std::vector<ReceivedMessage> KeeperChannel::takeReady()
{
    auto messages = std::vector<ReceivedMessage> {};
    while (!_ready.empty())
    {
        messages.push_back(std::move(_ready.front()));
        _ready.pop_front();
    }
    return messages;
}

void KeeperChannel::putBack(std::vector<ReceivedMessage> messages)
{
    for (auto& message: std::views::reverse(messages))
        _ready.push_front(std::move(message));
}

std::expected<std::size_t, ChannelError> KeeperChannel::sendSome(
    std::span<std::span<uint8_t const> const> bytes, std::span<int const> fds)
{
    return keeper::sendWithDescriptors(_socket.get(), bytes, fds)
        .transform_error([](keeper::SendError error) {
            return error == keeper::SendError::PeerGone ? ChannelError::Closed : ChannelError::Broken;
        });
}

std::expected<void, ChannelError> KeeperChannel::awaitWritable(core::platform::IClock const& clock,
                                                               core::platform::SteadyTimePoint deadline) const
{
    if (awaitEvents(_socket.get(), POLLOUT, clock, deadline) == Readiness::TimedOut)
        return std::unexpected { ChannelError::TimedOut };
    return {};
}

std::expected<void, ChannelError> KeeperChannel::sendNow(keeper::Message const& message,
                                                         std::span<int const> fds,
                                                         core::platform::IClock const& clock,
                                                         core::platform::SteadyTimePoint deadline)
{
    auto const bytes = frameBytesOf(message, fds.size());
    auto offset = std::size_t { 0 };
    while (offset < bytes.size())
    {
        auto const rest = std::span<uint8_t const> { bytes }.subspan(offset);
        auto const runs = std::array { rest };
        // The descriptors ride on the frame's first byte only.
        auto const sent = sendSome(runs, offset == 0 ? fds : std::span<int const> {});
        if (!sent)
            return std::unexpected { sent.error() };
        offset += *sent;
        if (*sent == 0)
            if (auto const writable = awaitWritable(clock, deadline); !writable)
                return writable;
    }
    return {};
}

} // namespace vthost::keeperlink
