// SPDX-License-Identifier: Apache-2.0
#include <core/net/SshAgent.hpp>

#include <core/net/detail/SshAgentEndpoint.hpp>
#include <core/net/detail/SshWire.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace core::net
{

namespace
{
    /// Message numbers of `draft-miller-ssh-agent`.
    enum class Message : std::uint8_t
    {
        Failure = 5,
        RequestIdentities = 11,
        IdentitiesAnswer = 12,
        SignRequest = 13,
        SignResponse = 14,
    };

    /// An agent's answer is a few keys' worth; OpenSSH itself refuses anything over 256 KiB.
    constexpr auto MaxReplySize = std::uint32_t { 256U * 1024U };
    constexpr auto FrameHeaderSize = std::size_t { 4 };

    using Reply = std::expected<std::vector<std::byte>, SshAgentError>;

    /// A request frame: length, type, body.
    std::vector<std::byte> frame(Message type, std::span<std::byte const> body)
    {
        auto out = std::vector<std::byte> {};
        detail::appendSshUint32(out, static_cast<std::uint32_t>(body.size() + 1));
        detail::appendSshByte(out, std::to_underlying(type));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    /// Reads exactly @p buffer.size() bytes; a hang-up or error is the agent being unavailable.
    async::Task<std::expected<void, SshAgentError>> readExactly(ISocket* socket, std::span<std::byte> buffer)
    {
        while (!buffer.empty())
        {
            auto const got = co_await socket->read(buffer);
            if (!got || *got == 0)
                co_return std::unexpected(SshAgentError::Unavailable);
            buffer = buffer.subspan(*got);
        }
        co_return std::expected<void, SshAgentError> {};
    }

    /// One request and its reply on a fresh connection.
    /// @return The reply's body after its type byte, whose type must be @p expected; `Failure` is Refused.
    async::Task<Reply> exchange(SshAgentConnector const* connect,
                                Message type,
                                std::vector<std::byte> body,
                                Message expected)
    {
        auto connected = co_await (*connect)();
        if (!connected)
            co_return std::unexpected(SshAgentError::Unavailable);
        auto const socket = std::move(*connected);

        auto const request = frame(type, body);
        auto const wrote = co_await socket->write(request);
        if (!wrote)
            co_return std::unexpected(SshAgentError::Unavailable);

        auto header = std::vector<std::byte>(FrameHeaderSize);
        if (auto const read = co_await readExactly(socket.get(), header); !read)
            co_return std::unexpected(read.error());
        auto const length = detail::SshWireReader { header }.uint32();
        if (!length || *length == 0 || *length > MaxReplySize)
            co_return std::unexpected(SshAgentError::Protocol);

        auto payload = std::vector<std::byte>(*length);
        if (auto const read = co_await readExactly(socket.get(), payload); !read)
            co_return std::unexpected(read.error());

        auto const answered = std::to_integer<std::uint8_t>(payload.front());
        payload.erase(payload.begin());
        if (answered == std::to_underlying(Message::Failure))
            co_return std::unexpected(SshAgentError::Refused);
        if (answered != std::to_underlying(expected))
            co_return std::unexpected(SshAgentError::Protocol);
        co_return payload;
    }

    /// Decodes an `SSH2_AGENT_IDENTITIES_ANSWER` body, dropping the keys it cannot use.
    std::expected<std::vector<SshPublicKey>, SshAgentError> decodeIdentities(std::span<std::byte const> body)
    {
        auto reader = detail::SshWireReader { body };
        auto const count = reader.uint32();
        if (!count)
            return std::unexpected(SshAgentError::Protocol);

        auto keys = std::vector<SshPublicKey> {};
        for ([[maybe_unused]] auto const index: std::views::iota(std::uint32_t { 0 }, *count))
        {
            auto const blob = reader.string();
            auto const comment = reader.string();
            if (!blob || !comment)
                return std::unexpected(SshAgentError::Protocol);

            // A key this module cannot use (another type, or not valid) is no failure of the
            // agent's: it is left out, as `ssh` leaves out what it cannot offer.
            auto key = parseSshPublicKeyBlob(*blob);
            if (!key)
                continue;
            key->comment.assign(reinterpret_cast<char const*>(comment->data()), comment->size());
            keys.push_back(std::move(*key));
        }
        if (!reader.atEnd())
            return std::unexpected(SshAgentError::Protocol);
        return keys;
    }

    class SshAgent final: public ISshAgent
    {
      public:
        explicit SshAgent(SshAgentConnector connector): _connector(std::move(connector)) {}

        async::Task<std::expected<std::vector<SshPublicKey>, SshAgentError>> identities() override
        {
            auto reply =
                co_await exchange(&_connector, Message::RequestIdentities, {}, Message::IdentitiesAnswer);
            if (!reply)
                co_return std::unexpected(reply.error());
            co_return decodeIdentities(*reply);
        }

        async::Task<std::expected<std::vector<std::byte>, SshAgentError>> sign(
            SshPublicKey const& key, std::span<std::byte const> data) override
        {
            // Not a coroutine itself: the request is built from the arguments now, so none of them
            // has to outlive the first suspension.
            auto body = std::vector<std::byte> {};
            detail::appendSshString(body, key.blob);
            detail::appendSshString(body, data);
            detail::appendSshUint32(body, 0); // flags: the key's native algorithm
            return signRequest(&_connector, std::move(body));
        }

      private:
        static async::Task<std::expected<std::vector<std::byte>, SshAgentError>> signRequest(
            SshAgentConnector const* connector, std::vector<std::byte> body)
        {
            auto reply =
                co_await exchange(connector, Message::SignRequest, std::move(body), Message::SignResponse);
            if (!reply)
                co_return std::unexpected(reply.error());

            auto reader = detail::SshWireReader { *reply };
            auto const signature = reader.string();
            if (!signature || !reader.atEnd())
                co_return std::unexpected(SshAgentError::Protocol);
            co_return std::vector<std::byte>(signature->begin(), signature->end());
        }

        SshAgentConnector _connector;
    };
} // namespace

std::unique_ptr<ISshAgent> makeSshAgent(SshAgentConnector connector)
{
    return std::make_unique<SshAgent>(std::move(connector));
}

std::unique_ptr<ISshAgent> makeSshAgent(EventLoop& loop, std::string endpoint)
{
    return makeSshAgent([loop = &loop, endpoint = std::move(endpoint)]() {
        return detail::connectAgentEndpoint(loop, endpoint);
    });
}

} // namespace core::net
