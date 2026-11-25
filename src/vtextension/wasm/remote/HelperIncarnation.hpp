// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One incarnation of an extension host, as the parent drives it: its control channel, its lanes and the
/// handle on it, from the handshake until it dies or is let go.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/LaneWatch.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace vtextension::wasm::remote
{

/// How many releases an incarnation queues for its control channel, at most: a host that has let this many
/// pile up is not reading it, and is killed.
inline constexpr std::size_t MaxPendingReleases = 4096;

/// Whether an incarnation still serves. It goes from Alive to Dead once, and never back.
enum class IncarnationState : uint8_t
{
    Dead,  ///< It closed a channel, failed one, or broke the protocol; it was terminated.
    Alive, ///< It answers.
};

/// What an incarnation holds its host to.
struct HelperPolicy
{
    HostTimeouts timeouts {};                                              ///< How long each step may take.
    LockdownRequirement lockdown = LockdownRequirement::RequireLockedDown; ///< Which hosts are driven at all.
};

/// The interrupt slots of one incarnation: one per live instance, InterruptSlotCount in all. Thread-safe.
///
/// A slot released is handed out again only once every other free one was: the later a slot comes back, the
/// longer an interrupt raised on it before has had to be taken by its host.
class InterruptSlots
{
  public:
    /// Takes a free slot.
    /// @return The slot; nothing if every slot is held.
    [[nodiscard]] std::optional<uint32_t> acquire();

    /// Gives a slot back. @param slot A slot acquire() handed out, lowered by now.
    void release(uint32_t slot) noexcept;

  private:
    /// Whether a slot is held.
    enum class SlotState : uint8_t
    {
        Free,
        Held,
    };

    std::mutex _mutex;                                              ///< Guards what follows.
    std::array<SlotState, transport::InterruptSlotCount> _slots {}; ///< Each slot's state.
    uint32_t _cursor = 0;                                           ///< Where the next search begins.
};

/// One incarnation of a host: started, handshaken, and holding the one runtime its parent created in it.
///
/// The control channel is shared by every thread, one request at a time under a mutex; each lane is used only
/// by the thread holding that lane of the kind (RemoteKindState), so it needs none. Once anything on any of
/// its channels goes wrong, the incarnation is dead for good: it is terminated, its channels are shut down so
/// that no thread stays blocked on them, and every later request fails.
///
/// Every control exchange runs under a step's watch (IHostHandle::watch()): a host that overstays it is
/// stopped, and the incarnation dies with it. So does one whose end the handle observes (HostAttachment).
class HelperIncarnation
{
  public:
    /// The id of the one runtime an incarnation holds.
    static constexpr uint32_t RuntimeId = 1;

    /// Starts a host, checks its Hello, and creates the runtime in it, within the handshake's budget.
    /// @param connector What starts the host.
    /// @param runtime The runtime to create; its id is RuntimeId.
    /// @param policy The budgets, and which confinement a host must report.
    /// @return The incarnation; `HostUnavailable` if the host could not be started, speaks another protocol
    ///         version, failed to confine itself or is not confined as @p policy requires, shares memory
    ///         where the parent copies it, did not create the runtime, or overstayed the handshake. A host
    ///         refused after it was started is terminated.
    [[nodiscard]] static std::expected<std::shared_ptr<HelperIncarnation>, ExtensionError> connect(
        IHostConnector& connector, proto::CreateRuntime const& runtime, HelperPolicy const& policy);

    /// Use connect(). Public for std::make_shared only.
    /// @param connection The started host's channels and handle; as many lanes as MaxLaneChannels.
    /// @param timeouts The budget of each step.
    HelperIncarnation(HostConnection connection, HostTimeouts timeouts);

    HelperIncarnation(HelperIncarnation const&) = delete;
    HelperIncarnation(HelperIncarnation&&) = delete;
    HelperIncarnation& operator=(HelperIncarnation const&) = delete;
    HelperIncarnation& operator=(HelperIncarnation&&) = delete;

    /// Lets the host go: its handle stops it.
    ~HelperIncarnation();

    /// Sends @p pdu on the control channel and receives the reply, as one request no other thread's
    /// interleaves with, under @p step's watch from the moment it holds the channel. A failure to send or
    /// receive, but an over-cap PDU the stream refused to send, kills the incarnation; so does overstaying.
    /// @param pdu The request.
    /// @param step The step it is: what its budget is.
    /// @return The reply; why there is none: `Closed` if the incarnation is dead already, or died meanwhile.
    [[nodiscard]] std::expected<proto::Pdu, transport::StreamError> request(proto::Pdu const& pdu,
                                                                            HostStep step);

    /// Tells the host a module or an instance is gone, without ever blocking: the release is queued, and
    /// sent at once only if no other thread holds the control channel and the channel takes it now. What is
    /// queued goes out before and after the next request's exchange, in order. Does nothing once the
    /// incarnation is dead; a host that lets more than MaxPendingReleases pile up kills it.
    /// @param release What is gone.
    void release(proto::Release release) noexcept;

    /// Lets the host go at once, as dropping the last reference to the incarnation would, though threads may
    /// still hold one: the incarnation is dead, its host is let go (IHostHandle::letGo()), and every channel
    /// is shut down, so that a thread blocked on one returns. Never blocks. Only the first of letGo() and
    /// markDead() does anything.
    void letGo() noexcept;

    /// @return Lane @p index's stream; used only by the thread holding that lane.
    [[nodiscard]] transport::PduStream& lane(std::size_t index) noexcept { return _lanes[index]; }

    /// @return Whether it still serves.
    [[nodiscard]] bool isAlive() const noexcept { return _state.load() == IncarnationState::Alive; }

    /// Declares it dead, unless it is: terminates the host, then shuts every channel down. Thread-safe;
    /// only the first of markDead() and letGo() does anything.
    void markDead() noexcept;

    /// Interrupts one of its instances, for good, without waiting on the host: arms the backstop on the lane
    /// a frame of it runs on, then raises its slot and wakes the host (IHostHandle::interrupt()). Wait-free.
    /// @param instanceId The instance's id.
    /// @param interruptSlot The slot it was instantiated with.
    void interrupt(uint32_t instanceId, uint32_t interruptSlot) noexcept;

    /// Lowers an instance's interrupt slot, once it is released, before the slot goes back to
    /// interruptSlots().
    /// @param interruptSlot The slot.
    void clearInterrupt(uint32_t interruptSlot) noexcept;

    /// @return The slots its instances are interrupted by.
    [[nodiscard]] InterruptSlots& interruptSlots() noexcept { return _interruptSlots; }

    /// @return Lane @p index's watch: the frames the thread holding the lane runs, for the backstop.
    [[nodiscard]] LaneWatch& laneWatch(std::size_t index) noexcept { return _laneWatches->lane(index); }

    /// @return A module id never used in this incarnation.
    [[nodiscard]] uint32_t nextModuleId() noexcept { return _nextModuleId.fetch_add(1); }

    /// @return An instance id never used in this incarnation.
    [[nodiscard]] uint32_t nextInstanceId() noexcept { return _nextInstanceId.fetch_add(1); }

  private:
    /// request()'s exchange, its caller holding the control mutex, under no watch of its own.
    [[nodiscard]] std::expected<proto::Pdu, transport::StreamError> exchange(proto::Pdu const& pdu);

    /// @return A watch over @p step, with its budget.
    [[nodiscard]] StepWatch watch(HostStep step);

    /// Sends every queued release, blocking, its caller holding the control mutex; kills the incarnation if
    /// one cannot be sent.
    /// @return Whether every one was sent.
    [[nodiscard]] bool sendReleases();

    /// Sends queued releases as far as the channel takes them now, its caller holding the control mutex;
    /// kills the incarnation if the channel failed.
    void trySendReleases() noexcept;

    /// @return The oldest queued release; nothing if none is queued.
    [[nodiscard]] std::optional<proto::Release> oldestRelease();

    /// Forgets the oldest queued release, which was sent.
    void forgetOldestRelease();

    /// Shuts every channel down.
    void shutdownChannels() noexcept;

    HostTimeouts _timeouts;
    std::mutex _controlMutex;           ///< Held for each request, and for sending queued releases.
    std::mutex _outboxMutex;            ///< Guards _outbox; never held across I/O.
    std::deque<proto::Release> _outbox; ///< Releases not sent yet, oldest first.
    transport::PduStream _control;
    std::vector<transport::PduStream> _lanes;
    std::atomic<IncarnationState> _state { IncarnationState::Alive };
    std::atomic<uint32_t> _nextModuleId { 1 };
    std::atomic<uint32_t> _nextInstanceId { 1 };
    InterruptSlots _interruptSlots;
    /// Shared with the handle's backstop (HostAttachment::lanes), which may look at it after the incarnation
    /// went.
    std::shared_ptr<LaneWatchSet> _laneWatches = std::make_shared<LaneWatchSet>();
    // Last, so that it goes first: the host is gone before its channels' parent ends close.
    std::unique_ptr<IHostHandle> _handle;
};

} // namespace vtextension::wasm::remote
