// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One incarnation of an extension host, as the parent drives it: its control channel, its lanes and the
/// handle on it, from the handshake until it dies or is let go.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace vtextension::wasm::remote
{

/// Whether an incarnation still serves. It goes from Alive to Dead once, and never back.
enum class IncarnationState : uint8_t
{
    Dead,  ///< It closed a channel, failed one, or broke the protocol; it was terminated.
    Alive, ///< It answers.
};

/// One incarnation of a host: started, handshaken, and holding the one runtime its parent created in it.
///
/// The control channel is shared by every thread, one request at a time under a mutex; each lane is used only
/// by the thread holding that lane of the kind (RemoteKindState), so it needs none. Once anything on any of
/// its channels goes wrong, the incarnation is dead for good: it is terminated, its channels are shut down so
/// that no thread stays blocked on them, and every later request fails.
class HelperIncarnation
{
  public:
    /// The id of the one runtime an incarnation holds.
    static constexpr uint32_t RuntimeId = 1;

    /// Starts a host, checks its Hello, and creates the runtime in it.
    /// @param connector What starts the host.
    /// @param runtime The runtime to create; its id is RuntimeId.
    /// @return The incarnation; `HostUnavailable` if the host could not be started, speaks another protocol
    ///         version, failed to confine itself, shares memory where the parent copies it, or did not create
    ///         the runtime. A host refused after it was started is terminated.
    [[nodiscard]] static std::expected<std::shared_ptr<HelperIncarnation>, ExtensionError> connect(
        IHostConnector& connector, proto::CreateRuntime const& runtime);

    /// Use connect(). Public for std::make_shared only.
    /// @param connection The started host's channels and handle; as many lanes as MaxLaneChannels.
    explicit HelperIncarnation(HostConnection connection);

    HelperIncarnation(HelperIncarnation const&) = delete;
    HelperIncarnation(HelperIncarnation&&) = delete;
    HelperIncarnation& operator=(HelperIncarnation const&) = delete;
    HelperIncarnation& operator=(HelperIncarnation&&) = delete;

    /// Lets the host go: its handle terminates it, and waits until it is gone.
    ~HelperIncarnation();

    /// Sends @p pdu on the control channel and receives the reply, as one request no other thread's
    /// interleaves with. A failure to send or receive, but an over-cap PDU the stream refused to send, kills
    /// the incarnation.
    /// @return The reply; why there is none: `Closed` if the incarnation is dead already.
    [[nodiscard]] std::expected<proto::Pdu, transport::StreamError> request(proto::Pdu const& pdu);

    /// Sends @p pdu on the control channel, expecting no reply; does nothing once the incarnation is dead,
    /// and ignores a failure.
    void post(proto::Pdu const& pdu) noexcept;

    /// @return Lane @p index's stream; used only by the thread holding that lane.
    [[nodiscard]] transport::PduStream& lane(std::size_t index) noexcept { return _lanes[index]; }

    /// @return Whether it still serves.
    [[nodiscard]] bool isAlive() const noexcept { return _state.load() == IncarnationState::Alive; }

    /// Declares it dead, unless it is: terminates the host, then shuts every channel down. Thread-safe;
    /// only the first call does anything.
    void markDead() noexcept;

    /// Interrupts one of its instances, for good, without waiting on the host.
    /// @param instanceId The instance's id; ignored by the host if unknown.
    void interrupt(uint32_t instanceId) noexcept;

    /// @return A module id never used in this incarnation.
    [[nodiscard]] uint32_t nextModuleId() noexcept { return _nextModuleId.fetch_add(1); }

    /// @return An instance id never used in this incarnation.
    [[nodiscard]] uint32_t nextInstanceId() noexcept { return _nextInstanceId.fetch_add(1); }

  private:
    std::mutex _controlMutex; ///< Held for each request and post on the control channel.
    transport::PduStream _control;
    std::vector<transport::PduStream> _lanes;
    std::atomic<IncarnationState> _state { IncarnationState::Alive };
    std::atomic<uint32_t> _nextModuleId { 1 };
    std::atomic<uint32_t> _nextInstanceId { 1 };
    // Last, so that it goes first: the host is gone before its channels' parent ends close.
    std::unique_ptr<IHostHandle> _handle;
};

} // namespace vtextension::wasm::remote
