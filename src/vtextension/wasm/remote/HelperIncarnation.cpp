// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/HelperIncarnation.hpp>

#include <expected>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

namespace vtextension::wasm::remote
{

using transport::PduStream;
using transport::StreamError;

namespace
{
    /// The limits the parent's ends apply: the protocol's own, which every host applies too.
    constexpr auto ParentLimits = proto::PduLimits {};

    /// @return Whether a host that said @p hello may be driven: it speaks this protocol, confined itself or
    ///         runs where there is no confinement, and copies memory, which is how this parent reaches it.
    [[nodiscard]] bool isAcceptable(proto::Hello const& hello) noexcept
    {
        return hello.version == proto::ProtocolVersion && hello.lockdown != proto::LockdownState::Failed
               && hello.memoryMode == proto::MemoryMode::Copy;
    }

    /// @return The parent's end of @p channel as a stream of @p kind.
    [[nodiscard]] PduStream parentEnd(transport::Channel channel, proto::ChannelKind kind)
    {
        return PduStream { std::move(channel), kind, proto::Direction::ToParent, ParentLimits };
    }
} // namespace

std::expected<std::shared_ptr<HelperIncarnation>, ExtensionError> HelperIncarnation::connect(
    IHostConnector& connector, proto::CreateRuntime const& runtime)
{
    auto connection = connector.connect();
    if (!connection)
        return std::unexpected(ExtensionError::HostUnavailable);
    if (connection->lanes.size() != MaxLaneChannels || !connection->handle)
    {
        if (connection->handle)
            connection->handle->terminate();
        return std::unexpected(ExtensionError::HostUnavailable);
    }
    auto incarnation = std::make_shared<HelperIncarnation>(std::move(*connection));

    // The host speaks first, unasked; a request then creates the runtime.
    auto const handshake = [&]() -> bool {
        auto const lock = std::scoped_lock { incarnation->_controlMutex };
        auto const hello = incarnation->_control.receive();
        return hello && std::holds_alternative<proto::Hello>(*hello)
               && isAcceptable(std::get<proto::Hello>(*hello));
    };
    auto const created = [&]() -> bool {
        auto request = runtime;
        request.runtimeId = RuntimeId;
        auto const reply = incarnation->request(request);
        return reply
               && *reply
                      == proto::Pdu { proto::RuntimeCreated { .runtimeId = RuntimeId,
                                                              .error = std::nullopt } };
    };
    if (!handshake() || !created())
    {
        incarnation->markDead();
        return std::unexpected(ExtensionError::HostUnavailable);
    }
    return incarnation;
}

HelperIncarnation::HelperIncarnation(HostConnection connection):
    _control { parentEnd(std::move(connection.control), proto::ChannelKind::Control) },
    _handle { std::move(connection.handle) }
{
    _lanes.reserve(connection.lanes.size());
    for (auto& lane: connection.lanes)
        _lanes.push_back(parentEnd(std::move(lane), proto::ChannelKind::Lane));
}

HelperIncarnation::~HelperIncarnation() = default;

std::expected<proto::Pdu, StreamError> HelperIncarnation::request(proto::Pdu const& pdu)
{
    auto const lock = std::scoped_lock { _controlMutex };
    if (!isAlive())
        return std::unexpected(StreamError::Closed);
    if (auto const sent = _control.send(pdu); !sent)
    {
        // A PDU over its cap was not sent, and the stream is intact: the request fails, the host lives on.
        if (sent.error() != StreamError::OverCap)
            markDead();
        return std::unexpected(sent.error());
    }
    auto reply = _control.receive();
    if (!reply)
        markDead();
    return reply;
}

void HelperIncarnation::post(proto::Pdu const& pdu) noexcept
{
    auto const lock = std::scoped_lock { _controlMutex };
    if (isAlive())
        std::ignore = _control.send(pdu);
}

void HelperIncarnation::markDead() noexcept
{
    if (_state.exchange(IncarnationState::Dead) == IncarnationState::Dead)
        return;
    _handle->terminate();
    // A thread blocked on one of the parent's ends returns; a host that never closes its own ends, as a
    // hostile one may not, cannot keep it waiting.
    _control.shutdown();
    for (auto& lane: _lanes)
        lane.shutdown();
}

void HelperIncarnation::interrupt(uint32_t instanceId) noexcept
{
    if (isAlive())
        _handle->interrupt(instanceId);
}

} // namespace vtextension::wasm::remote
