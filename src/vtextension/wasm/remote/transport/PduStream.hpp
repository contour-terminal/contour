// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// PDUs over a Channel: frames what it sends, and reads whole frames back, refusing any the codec refuses.
///
/// A handle may travel with a PDU (HandleGrant.hpp): on POSIX as a descriptor attached to its frame. Only the
/// stream the helper reads its control channel with takes one in, and holds at most one until it is claimed;
/// every other stream leaves the system to discard what a peer attaches, so that Contour installs nothing a
/// helper sends.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>

namespace vtextension::wasm::remote::transport
{

/// Why a stream could not send or receive a PDU. Every one ends the stream, except an OverCap from send() or
/// trySend(), which sent nothing.
enum class StreamError : uint8_t
{
    Closed,    ///< The channel closed, or was shut down.
    Io,        ///< The channel failed.
    Malformed, ///< The peer sent a frame the codec refuses: a malformed one, or one of the wrong kind,
               ///< direction or channel.
    OverCap,   ///< The peer sent a frame larger than its kind may be; or, from send(), the PDU holds a
               ///< list over its cap or is larger than its kind may be, and was not sent.
};

struct HandleGrant;

/// What became of a PDU a stream was asked to send without blocking.
enum class SendOutcome : uint8_t
{
    WouldBlock, ///< Nothing of it was sent: the channel takes nothing now, or a frame sent before is not all
                ///< sent yet. It is the caller's to send again.
    Sent,       ///< It is the stream's: sent, or begun and its tail kept for the next send.
};

/// What a stream's receive buffer cost so far; a test hook.
struct ReceiveBufferStats
{
    std::size_t allocations = 0; ///< How often the buffer was allocated anew.
    std::size_t peakBytes = 0;   ///< The most bytes it held allocated at once.
};

/// One channel's PDUs. Owns the channel and a receive buffer; used by one thread at a time, which may both
/// send and receive.
class PduStream
{
  public:
    /// Whether a stream takes in the handles a peer attaches.
    enum class Attachments : uint8_t
    {
        Discarded, ///< It leaves the system to discard them: every stream but one.
        Taken,     ///< It takes them in: the helper's control stream, on POSIX.
    };

    /// @param channel The channel it reads and writes.
    /// @param kind Which channel it is: a PDU of the other is refused.
    /// @param arriving The way PDUs it receives travel: a PDU travelling the other way is refused.
    /// @param limits The caps of the PDUs whose size follows the stream's limits.
    PduStream(Channel channel, proto::ChannelKind kind, proto::Direction arriving, proto::PduLimits limits);

    /// Sends one PDU, blocking until the channel took it -- and first the tail of a frame trySend() began.
    /// @param pdu The PDU.
    /// @return Nothing once it is sent; why it was not. `OverCap` if a list it carries exceeds its cap
    ///         (Pdu.hpp), or its body exceeds its kind's byte cap under the stream's limits (PduTable.hpp):
    ///         nothing was sent, and the stream remains usable.
    [[nodiscard]] std::expected<void, StreamError> send(proto::Pdu const& pdu);

    /// Sends one PDU as send() does, with what @p grant attaches travelling with its frame: on POSIX a
    /// duplicate of its descriptor, on the frame's first byte. A grant that attaches nothing (Windows, where
    /// the handle's value is in the PDU) sends it as send() does.
    /// @param pdu The PDU the grant belongs to: on the control channel, the Instantiate that names it.
    /// @param grant What grantHandle() made; its descriptor stays the caller's.
    /// @return Nothing once it is sent; why it was not, as send() fails.
    [[nodiscard]] std::expected<void, StreamError> send(proto::Pdu const& pdu, HandleGrant const& grant);

    /// Sends one PDU as far as the channel takes it now, without blocking: a frame begun is the stream's, and
    /// the rest of it is sent first by the next send, blocking or not. A frame begun before and not all sent
    /// yet is sent on first; while it is not, nothing of @p pdu is.
    /// @param pdu The PDU.
    /// @return `Sent` once the frame is begun, `WouldBlock` if nothing of it was sent; why it failed, as
    ///         send() fails.
    [[nodiscard]] std::expected<SendOutcome, StreamError> trySend(proto::Pdu const& pdu);

    /// Receives the next PDU, blocking until all of it has arrived. A frame larger than its kind may be fails
    /// as soon as its header arrives, without waiting for its body.
    /// @return The PDU; why none could be received.
    [[nodiscard]] std::expected<proto::Pdu, StreamError> receive();

    /// Takes the handle that arrived with a PDU received, which the stream held until now; see
    /// claimGrantedHandle(), which is how a reader claims it.
    /// @return The handle; none if none arrived since the last one was taken.
    [[nodiscard]] OwnedHandle takeAttachedHandle() noexcept;

    /// Shuts the channel down: a receive blocked on it, from any thread, fails `Closed`.
    void shutdown() noexcept;

    /// Test hook.
    /// @return What the receive buffer cost since the stream was made.
    [[nodiscard]] ReceiveBufferStats receiveBufferStats() const noexcept { return _stats; }

    /// Test hook.
    /// @return How many bytes of a frame trySend() began are not sent yet.
    [[nodiscard]] std::size_t unsentBytes() const noexcept { return _unsentTail.size(); }

  private:
    /// Replaces the receive buffer with one of @p capacity bytes, keeping the bytes buffered.
    /// @param capacity At least _buffered; 0 lets the buffer go.
    void reallocate(std::size_t capacity);

    /// @return @p pdu as one frame; `OverCap` if the peer would refuse it (send()).
    [[nodiscard]] std::expected<std::vector<uint8_t>, StreamError> frameOf(proto::Pdu const& pdu) const;

    /// Sends @p pdu, blocking, with @p attached travelling on its frame's first byte if there is one: always
    /// nothing on Windows, where nothing travels beside a frame.
    [[nodiscard]] std::expected<void, StreamError> sendAttaching(proto::Pdu const& pdu,
                                                                 std::optional<NativeHandle> attached);

    /// Sends the tail of a frame trySend() began, if any.
    [[nodiscard]] std::expected<void, StreamError> flushUnsentTail();

    /// Receives more bytes into the buffer's free space, and takes in a handle that came with them where this
    /// stream takes handles.
    /// @return How many bytes arrived; `Malformed` for a handle while one is unclaimed, or more than one at
    ///         once.
    [[nodiscard]] std::expected<std::size_t, StreamError> receiveMore();

    Channel _channel;
    proto::ChannelKind _kind;
    proto::Direction _arriving;
    proto::PduLimits _limits;
    std::unique_ptr<uint8_t[]> _buffer; ///< Received bytes; the first _buffered of them are not consumed yet.
    std::size_t _capacity = 0;          ///< How many bytes _buffer holds.
    std::size_t _buffered = 0;
    ReceiveBufferStats _stats;
    std::vector<uint8_t> _unsentTail; ///< What trySend() did not get sent of the frame it began.
    Attachments _attachments;         ///< Whether it takes handles in: fixed by its channel and direction.
    OwnedHandle _attached;            ///< A handle taken in, until it is claimed.
};

} // namespace vtextension::wasm::remote::transport
