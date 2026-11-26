// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/HelperIncarnation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <tuple>
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

    /// One step's budget, as HostTimeouts holds it.
    struct StepBudget
    {
        HostStep step;
        std::chrono::milliseconds HostTimeouts::* budget;
    };

    /// Every step's budget, at its step's index.
    constexpr auto StepBudgets = std::array {
        StepBudget { .step = HostStep::Handshake, .budget = &HostTimeouts::handshake },
        StepBudget { .step = HostStep::Load, .budget = &HostTimeouts::load },
        StepBudget { .step = HostStep::Instantiate, .budget = &HostTimeouts::instantiate },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, StepBudgets.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(StepBudgets[index].step) == index;
                                      }));

    /// @return Why a host confined as @p state is not driven under @p requirement; nothing if it is. One that
    ///         failed to confine itself never is.
    [[nodiscard]] constexpr std::optional<HostRefusal> lockdownRefusalOf(
        proto::LockdownState state, LockdownRequirement requirement) noexcept
    {
        switch (state)
        {
            case proto::LockdownState::Failed: return HostRefusal::LockdownFailed;
            case proto::LockdownState::Unsandboxed:
                if (requirement == LockdownRequirement::AcceptUnsandboxed)
                    return std::nullopt;
                return HostRefusal::Unsandboxed;
            case proto::LockdownState::LockedDown: return std::nullopt;
        }
        return HostRefusal::LockdownFailed;
    }

    /// @return Why a host that said @p hello is not driven; nothing if it is: it speaks this protocol, and is
    ///         confined as @p requirement asks. Whether it copies memory or shares it, both are driven.
    [[nodiscard]] std::optional<HostRefusal> refusalOf(proto::Hello const& hello,
                                                       LockdownRequirement requirement) noexcept
    {
        if (hello.version != proto::ProtocolVersion)
            return HostRefusal::ProtocolVersion;
        return lockdownRefusalOf(hello.lockdown, requirement);
    }

    /// @return The parent's end of @p channel as a stream of @p kind.
    [[nodiscard]] PduStream parentEnd(transport::Channel channel, proto::ChannelKind kind)
    {
        return PduStream { std::move(channel), kind, proto::Direction::ToParent, ParentLimits };
    }
} // namespace

std::expected<std::shared_ptr<HelperIncarnation>, ExtensionError> HelperIncarnation::connect(
    IHostConnector& connector,
    proto::CreateRuntime const& runtime,
    HelperPolicy const& policy,
    HostRefusalLog& refusals)
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
    auto incarnation = std::make_shared<HelperIncarnation>(std::move(*connection), policy.timeouts);
    // A host that ends is declared dead at once, rather than by the next request that finds its channel
    // closed; one that is gone already, at once.
    // Its lanes are watched from now on: an interrupted call that does not end within the grace has the host
    // stopped, and so the incarnation dead.
    incarnation->_handle->attach(HostAttachment {
        .onDeath =
            [weak = std::weak_ptr { incarnation }] {
                if (auto const self = weak.lock())
                    self->markDead();
            },
        .lanes = incarnation->_laneWatches,
        .interruptGrace = policy.timeouts.interruptGrace,
    });

    // The host speaks first, unasked; a request then creates the runtime. Both within the handshake's budget.
    auto const handshake = [&]() -> bool {
        auto const lock = std::scoped_lock { incarnation->_controlMutex };
        auto const watch = incarnation->watch(HostStep::Handshake);
        auto const hello = incarnation->_control.receive();
        if (!hello || !std::holds_alternative<proto::Hello>(*hello))
            return false;
        auto const& said = std::get<proto::Hello>(*hello);
        if (auto const refusal = refusalOf(said, policy.lockdown))
        {
            refusals.refused(*refusal,
                             proto::LockdownFailure { .step = said.failedStep, .code = said.failureCode });
            connector.refused(*refusal);
            return false;
        }
        // Confined, but by less than the sandbox could: said once, however many hosts say it.
        refusals.unavailable(said.unavailableStep);
        incarnation->_memoryMode = said.memoryMode;
        auto request = runtime;
        request.runtimeId = RuntimeId;
        auto const reply = incarnation->exchange(request, {}, nullptr);
        return reply
               && *reply
                      == proto::Pdu { proto::RuntimeCreated { .runtimeId = RuntimeId,
                                                              .error = std::nullopt } };
    };
    if (!handshake())
    {
        incarnation->markDead();
        return std::unexpected(ExtensionError::HostUnavailable);
    }
    return incarnation;
}

HelperIncarnation::HelperIncarnation(HostConnection connection, HostTimeouts timeouts):
    _timeouts { timeouts },
    _objectBytesCap { connection.objectBytesCap },
    _control { parentEnd(std::move(connection.control), proto::ChannelKind::Control) },
    _handle { std::move(connection.handle) }
{
    _lanes.reserve(connection.lanes.size());
    for (auto& lane: connection.lanes)
        _lanes.push_back(parentEnd(std::move(lane), proto::ChannelKind::Lane));
}

HelperIncarnation::~HelperIncarnation() = default;

std::expected<proto::Pdu, StreamError> HelperIncarnation::request(proto::Pdu const& pdu, HostStep step)
{
    return requestHolding(pdu, step, {}, nullptr);
}

std::expected<proto::Pdu, StreamError> HelperIncarnation::request(proto::Pdu const& pdu,
                                                                  HostStep step,
                                                                  transport::HandleGrant const& grant,
                                                                  GrowAnswer const& answer)
{
    return requestHolding(pdu, step, grant, &answer);
}

std::expected<proto::Pdu, StreamError> HelperIncarnation::requestHolding(proto::Pdu const& pdu,
                                                                         HostStep step,
                                                                         transport::HandleGrant const& grant,
                                                                         GrowAnswer const* answer)
{
    auto reply = [&]() -> std::expected<proto::Pdu, StreamError> {
        auto const lock = std::scoped_lock { _controlMutex };
        // Armed only once the channel is this request's: waiting for another's is not the host's time.
        auto const watch = this->watch(step);
        if (!isAlive())
            return std::unexpected(StreamError::Closed);
        // What was released before goes first -- an interrupt slot given back with an instance is handed to
        // the next only after the host heard of it -- and what was released meanwhile goes right after.
        if (!sendReleases())
            return std::unexpected(StreamError::Closed);
        auto exchanged = exchange(pdu, grant, answer);
        if (exchanged)
            std::ignore = sendReleases();
        return exchanged;
    }();
    // A release queued after the last send above, while the channel was still held -- the watch's disarm
    // runs then -- found it taken: it is this thread's to send.
    offerReleases();
    return reply;
}

std::expected<proto::Pdu, StreamError> HelperIncarnation::exchange(proto::Pdu const& pdu,
                                                                   transport::HandleGrant const& grant,
                                                                   GrowAnswer const* answer)
{
    if (!isAlive())
        return std::unexpected(StreamError::Closed);
    if (auto const sent = _control.send(pdu, grant); !sent)
    {
        // A PDU over its cap was not sent, and the stream is intact: the request fails, the host lives on.
        if (sent.error() != StreamError::OverCap)
            markDead();
        return std::unexpected(sent.error());
    }
    auto reply = _control.receive();
    // Growth the request's own work asks for is answered here, on the channel the request holds: nothing
    // else is sent on it until the reply came.
    while (answer && reply && std::holds_alternative<proto::Grow>(*reply))
    {
        auto const verdict = (*answer)(std::get<proto::Grow>(*reply));
        if (verdict == GrowVerdict::Violation)
        {
            markDead();
            return std::unexpected(StreamError::Malformed);
        }
        if (auto const sent = _control.send(proto::Grown { .status = growStatusOf(verdict) }); !sent)
        {
            markDead();
            return std::unexpected(sent.error());
        }
        reply = _control.receive();
    }
    if (!reply)
        markDead();
    return reply;
}

std::optional<ObjectBytesLease> HelperIncarnation::leaseObjectBytes(uint64_t bytes) noexcept
{
    auto live = _liveObjectBytes.load();
    do
    {
        if (_objectBytesCap != 0 && (bytes > _objectBytesCap || live > _objectBytesCap - bytes))
            return std::nullopt;
    } while (!_liveObjectBytes.compare_exchange_weak(live, live + bytes));
    return ObjectBytesLease { *this, bytes };
}

void HelperIncarnation::returnObjectBytes(uint64_t bytes) noexcept
{
    _liveObjectBytes.fetch_sub(bytes);
}

ObjectBytesLease::~ObjectBytesLease()
{
    if (_incarnation)
        _incarnation->returnObjectBytes(_bytes);
}

StepWatch HelperIncarnation::watch(HostStep step)
{
    return _handle->watch(step, _timeouts.*StepBudgets[std::to_underlying(step)].budget);
}

void HelperIncarnation::release(proto::Release release) noexcept
{
    if (!isAlive())
        return;
    auto const isOverflowing = [&] {
        auto const lock = std::scoped_lock { _outboxMutex };
        if (_outbox.size() >= MaxPendingReleases)
            return true;
        _outbox.push_back(release);
        return false;
    }();
    if (isOverflowing)
    {
        markDead();
        return;
    }
    offerReleases();
}

void HelperIncarnation::offerReleases() noexcept
{
    // Sent now only if that costs no wait. A thread that finds the channel taken leaves its release to the
    // holder, and every holder looks at the queue again once it let go, so none is left behind for a request
    // that may never come. One that the channel does not take now waits for the next release or request.
    while (isAlive() && hasQueuedReleases())
    {
        auto const lock = std::unique_lock { _controlMutex, std::try_to_lock };
        if (!lock.owns_lock() || trySendReleases() == ReleaseFlush::Blocked)
            return;
    }
}

bool HelperIncarnation::hasQueuedReleases()
{
    auto const lock = std::scoped_lock { _outboxMutex };
    return !_outbox.empty();
}

bool HelperIncarnation::sendReleases()
{
    while (auto const next = oldestRelease())
    {
        if (!_control.send(*next))
        {
            markDead();
            return false;
        }
        forgetOldestRelease();
    }
    return true;
}

HelperIncarnation::ReleaseFlush HelperIncarnation::trySendReleases() noexcept
{
    while (auto const next = oldestRelease())
    {
        auto const outcome = _control.trySend(*next);
        if (!outcome)
        {
            markDead();
            return ReleaseFlush::Blocked;
        }
        if (*outcome == transport::SendOutcome::WouldBlock)
            return ReleaseFlush::Blocked;
        forgetOldestRelease();
    }
    return ReleaseFlush::Drained;
}

std::optional<proto::Release> HelperIncarnation::oldestRelease()
{
    auto const lock = std::scoped_lock { _outboxMutex };
    return _outbox.empty() ? std::nullopt : std::optional { _outbox.front() };
}

void HelperIncarnation::forgetOldestRelease()
{
    // Only the holder of the control mutex takes from the front; others only append.
    auto const lock = std::scoped_lock { _outboxMutex };
    _outbox.pop_front();
}

void HelperIncarnation::markDead() noexcept
{
    if (_state.exchange(IncarnationState::Dead) == IncarnationState::Dead)
        return;
    _handle->terminate();
    shutdownChannels();
}

void HelperIncarnation::letGo() noexcept
{
    if (_state.exchange(IncarnationState::Dead) == IncarnationState::Dead)
        return;
    _handle->letGo();
    shutdownChannels();
}

void HelperIncarnation::shutdownChannels() noexcept
{
    // A thread blocked on one of the parent's ends returns; a host that never closes its own ends, as a
    // hostile one may not, cannot keep it waiting.
    _control.shutdown();
    for (auto& lane: _lanes)
        lane.shutdown();
}

void HelperIncarnation::interrupt(uint32_t instanceId, uint32_t interruptSlot) noexcept
{
    if (!isAlive())
        return;
    // Armed before the backstop is woken to look.
    _laneWatches->interrupt(instanceId);
    _handle->interrupt(interruptSlot);
}

void HelperIncarnation::clearInterrupt(uint32_t interruptSlot) noexcept
{
    _handle->clearInterrupt(interruptSlot);
}

std::optional<uint32_t> InterruptSlots::acquire()
{
    auto const lock = std::scoped_lock { _mutex };
    for (auto const offset: std::views::iota(uint32_t { 0 }, transport::InterruptSlotCount))
    {
        auto const slot = (_cursor + offset) % transport::InterruptSlotCount;
        if (_slots[slot] == SlotState::Free)
        {
            _slots[slot] = SlotState::Held;
            _cursor = (slot + 1) % transport::InterruptSlotCount;
            return slot;
        }
    }
    return std::nullopt;
}

void InterruptSlots::release(uint32_t slot) noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    if (slot < transport::InterruptSlotCount)
        _slots[slot] = SlotState::Free;
}

} // namespace vtextension::wasm::remote
