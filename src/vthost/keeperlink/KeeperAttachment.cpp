// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/KeeperAttachment.hpp>

#include <sys/socket.h>

#include <optional>
#include <utility>
#include <variant>

#include <unistd.h>

#include <vthost/keeper/Codec.hpp>

namespace vthost::keeperlink
{

namespace
{
    [[nodiscard]] AttachError attachErrorOf(ChannelError error) noexcept
    {
        switch (error)
        {
            case ChannelError::Closed: return AttachError::Closed;
            case ChannelError::TimedOut: return AttachError::TimedOut;
            case ChannelError::Broken: break;
        }
        return AttachError::Broken;
    }

    /// @return Whether @p fd is an AF_UNIX stream socket, as a keeper's channel is.
    [[nodiscard]] bool isStreamSocket(int fd) noexcept
    {
        auto type = 0;
        auto size = static_cast<socklen_t>(sizeof(type));
        if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM)
            return false;
        auto address = ::sockaddr_storage {};
        auto length = static_cast<socklen_t>(sizeof(address));
        return ::getsockname(fd, reinterpret_cast<::sockaddr*>(&address), &length) == 0
               && address.ss_family == AF_UNIX;
    }

    /// This daemon's channel version.
    constexpr auto OwnVersion = keeper::ChannelVersion;
} // namespace

std::string_view describe(AttachError error) noexcept
{
    switch (error)
    {
        case AttachError::Closed: return "the keeper closed the channel";
        case AttachError::Broken: return "the keeper's channel broke";
        case AttachError::TimedOut: return "the keeper did not answer in time";
        case AttachError::NotAChannel: return "the descriptor is no keeper's channel";
    }
    return "the keeper's channel failed";
}

std::expected<KeeperAttachment, AttachError> attachToKeeper(keeper::UniqueFd socket,
                                                            core::platform::IClock const& clock,
                                                            std::chrono::milliseconds bound)
{
    if (!isStreamSocket(socket.get()))
        return std::unexpected { AttachError::NotAChannel };
    auto const deadline = clock.now() + bound;
    auto channel = KeeperChannel { std::move(socket) };
    if (auto const said = channel.sendNow(keeper::Hello {}, {}, clock, deadline); !said)
        return std::unexpected { attachErrorOf(said.error()) };

    auto peerVersion = std::optional<uint32_t> {};
    auto launch = std::optional<keeper::Launch> {};
    auto early = std::vector<ReceivedMessage> {};
    while (!peerVersion || !launch)
    {
        auto received = channel.receive(clock, deadline);
        if (!received)
            return std::unexpected { attachErrorOf(received.error()) };
        if (received->message)
        {
            if (auto const* hello = std::get_if<keeper::Hello>(&*received->message))
            {
                peerVersion = hello->version;
                continue;
            }
            if (auto* const launched = std::get_if<keeper::Launch>(&*received->message))
            {
                launch = std::move(*launched);
                continue;
            }
        }
        early.push_back(std::move(*received));
    }

    auto attachment =
        KeeperAttachment { .channel = std::move(channel),
                           .continuity = launch->continuity,
                           .options = std::move(launch->options),
                           .version = keeper::negotiate(OwnVersion, *peerVersion),
                           .keeperProcessId = static_cast<int64_t>(::getppid()),
                           .childDescriptorLimit = launch->originalDescriptorLimit != 0
                                                       ? std::optional { launch->originalDescriptorLimit }
                                                       : std::nullopt,
                           .inventory = {} };

    if (attachment.continuity == keeper::LaunchContinuity::Recovered)
    {
        if (auto const asked = attachment.channel.sendNow(keeper::Adopt {}, {}, clock, deadline); !asked)
            return std::unexpected { attachErrorOf(asked.error()) };
        auto begun = false;
        while (true)
        {
            auto received = attachment.channel.receive(clock, deadline);
            if (!received)
                return std::unexpected { attachErrorOf(received.error()) };
            auto const isBegin =
                received->message && std::holds_alternative<keeper::InventoryBegin>(*received->message);
            auto const isEnd =
                received->message && std::holds_alternative<keeper::InventoryEnd>(*received->message);
            begun = begun || isBegin;
            if (!begun)
            {
                early.push_back(std::move(*received));
                continue;
            }
            attachment.inventory.push_back(std::move(*received));
            if (isEnd)
                break;
        }
    }

    attachment.channel.putBack(std::move(early));
    return attachment;
}

} // namespace vthost::keeperlink
