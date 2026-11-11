// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <vthost/keeper/Frame.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>

/// The daemon's side of the channel to its PTY keeper: the link (KeeperLink), the handshake that comes
/// before it (attachToKeeper()), and the socket both speak over (KeeperChannel).
namespace vthost::keeperlink
{

/// A frame that is no message this daemon can read -- a newer keeper's: an unknown tag, an enum value beyond
/// the wire table, more descriptors than the message may carry. Answered `Unsupported{tag}`; its descriptors
/// are closed (keeper::failureOf()).
struct UnreadableMessage
{
    uint32_t tag = 0;
};

/// One message the keeper sent, with the descriptors that came with it.
struct ReceivedMessage
{
    std::expected<keeper::Message, UnreadableMessage> message; ///< The message, or the tag of one not read.
    std::vector<keeper::UniqueFd> fds;                         ///< Now the daemon's.
};

/// Why the channel to the keeper is no longer usable.
enum class ChannelError : uint8_t
{
    Closed = 0, ///< The keeper closed its end.
    Broken,     ///< A read failed, descriptors were truncated, or a frame is not one of the channel's.
    TimedOut,   ///< A blocking call ran out of time.
};

/// The daemon's end of the channel: an AF_UNIX stream socket carrying frames (@see keeper::FrameAssembler)
/// and their descriptors.
///
/// Reads never block unless asked to (receive()). Received descriptors are close-on-exec; a truncated
/// descriptor list (MSG_CTRUNC) breaks the channel. Sends never raise SIGPIPE (MSG_NOSIGNAL; SO_NOSIGPIPE
/// where there is none). The socket is made close-on-exec, so no child of the daemon inherits it; a live
/// upgrade's exec keeps it across explicitly, as the descriptor the new image finds the keeper on.
///
/// Not thread-safe: one thread at a time.
class KeeperChannel
{
  public:
    /// @param socket A connected AF_UNIX stream socket; made non-blocking here.
    explicit KeeperChannel(keeper::UniqueFd socket);

    /// @return The socket, to wait on.
    [[nodiscard]] int handle() const noexcept { return _socket.get(); }

    /// Reads what the socket holds now, without blocking.
    /// @return The messages it completed, in order (possibly none); or why the channel is unusable.
    [[nodiscard]] std::expected<std::vector<ReceivedMessage>, ChannelError> readAvailable();

    /// Waits for the next message.
    /// @param clock The clock @p deadline is on.
    /// @param deadline When to give up.
    /// @return The message; or why none came.
    [[nodiscard]] std::expected<ReceivedMessage, ChannelError> receive(
        core::platform::IClock const& clock, core::platform::SteadyTimePoint deadline);

    /// @return The messages read already and not yet handed out, in order; none are left.
    [[nodiscard]] std::vector<ReceivedMessage> takeReady();

    /// Puts @p messages back in front of what is still to be read, for the next reader.
    /// @param messages Messages taken from this channel, in their order.
    void putBack(std::vector<ReceivedMessage> messages);

    /// Sends what of @p bytes the socket takes now, with @p fds on the first byte. Never blocks.
    /// @param bytes The bytes.
    /// @param fds Descriptors to send with them; they stay the caller's. At most keeper::MaxFdsPerFrame.
    /// @return How many bytes went (0 when the socket takes nothing now); or why the send failed
    ///         (ChannelError::Closed when the keeper is gone).
    [[nodiscard]] std::expected<std::size_t, ChannelError> sendSome(
        std::span<std::span<uint8_t const> const> bytes, std::span<int const> fds);

    /// Sends @p message whole, waiting while the socket is full.
    /// @param message The message.
    /// @param fds Descriptors to send with it; they stay the caller's.
    /// @param clock The clock @p deadline is on.
    /// @param deadline When to give up.
    /// @return Nothing once sent; or why it could not be.
    [[nodiscard]] std::expected<void, ChannelError> sendNow(keeper::Message const& message,
                                                            std::span<int const> fds,
                                                            core::platform::IClock const& clock,
                                                            core::platform::SteadyTimePoint deadline);

    /// Waits until the socket can take more bytes.
    /// @param clock The clock @p deadline is on.
    /// @param deadline When to give up.
    /// @return Nothing once it can; or ChannelError::TimedOut.
    [[nodiscard]] std::expected<void, ChannelError> awaitWritable(
        core::platform::IClock const& clock, core::platform::SteadyTimePoint deadline) const;

  private:
    /// Reads once into the assembler.
    /// @return The bytes read (0 at the end); nullopt when none are there now; or why the channel is
    /// unusable.
    [[nodiscard]] std::expected<std::optional<std::size_t>, ChannelError> readOnce();
    /// Moves the frames the assembler completed into _ready.
    [[nodiscard]] std::expected<void, ChannelError> collect();

    keeper::UniqueFd _socket;
    keeper::FrameAssembler _assembler;
    std::vector<uint8_t> _readBuffer;
    std::deque<ReceivedMessage> _ready;
};

/// @param message A message.
/// @param fdCount How many descriptors ride with it.
/// @return It as one frame of the channel: header and body.
[[nodiscard]] std::vector<uint8_t> frameBytesOf(keeper::Message const& message, std::size_t fdCount);

} // namespace vthost::keeperlink
