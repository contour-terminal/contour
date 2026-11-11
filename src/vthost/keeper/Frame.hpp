// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <vthost/keeper/Messages.hpp>

namespace vthost::keeper
{

/// The most bytes one frame's body may hold; a peer announcing more is a protocol error.
inline constexpr std::size_t MaxFrameBodySize = std::size_t { 16 } * 1024 * 1024;

/// One frame of the channel: a body and the descriptors that rode with its first byte.
struct Frame
{
    std::vector<uint8_t> body;
    std::vector<int> fds; ///< Not owned by the frame; whoever receives them closes them.
};

/// Why a stream of frames is broken. The link ends on any of them.
enum class FrameError : uint8_t
{
    MalformedHeader = 0, ///< A header varint is overlong.
    BodyTooLarge,        ///< The header announces more than MaxFrameBodySize.
    TooManyDescriptors,  ///< The header announces more than MaxFdsPerFrame.
    DescriptorMismatch,  ///< The descriptors that arrived are not the count the header announces.
};

/// Writes a frame's header and body.
/// @param frame The frame; its descriptors travel beside the bytes, not in them.
/// @return `varint length, varint fdCount, body`.
[[nodiscard]] std::vector<uint8_t> encodeFrame(Frame const& frame);

/// Cuts a stream of reads into frames, reuniting each with the descriptors that arrived with its bytes.
///
/// Contract: descriptors belong to the frame whose first byte arrived in the same read, and that announces
/// them. A stream socket may glue a plain frame to a following descriptor-carrying one in one read (Linux
/// does: a read stops after the bytes that carried descriptors, not before), so a read's descriptors go to
/// the first frame starting in it that announces any. The sender therefore sends each descriptor-carrying
/// frame with its own `sendmsg`, its descriptors on its first byte. Descriptors that no frame starting in
/// their read announces, or that a frame announces in a different number, break the stream.
class FrameAssembler
{
  public:
    /// Adds one read.
    /// @param bytes What the read returned.
    /// @param fds   The descriptors that arrived with its first byte; the assembler does not own them.
    void feed(std::span<uint8_t const> bytes, std::span<int const> fds);

    /// @return The next complete frame, or nothing when more bytes are needed or the stream is broken.
    [[nodiscard]] std::optional<Frame> next();

    /// @return Why the stream is broken, if it is; no frame comes out after that.
    [[nodiscard]] std::optional<FrameError> error() const noexcept { return _error; }

    /// @return How many bytes were fed that no frame handed out yet: a frame begun but not complete.
    ///         Zero on a frame boundary.
    [[nodiscard]] std::size_t pendingBytes() const noexcept { return _buffer.size(); }

    /// @return The descriptors fed but not handed out in a frame, for the caller to close.
    [[nodiscard]] std::vector<int> takePendingFds();

  private:
    /// The descriptors of one read, and the bytes that read brought.
    struct Arrival
    {
        std::size_t begin = 0; ///< Where in _buffer the read's bytes start; 0 once its start was consumed.
        std::size_t end = 0;   ///< Where in _buffer they end.
        std::vector<int> fds;
    };

    std::vector<uint8_t> _buffer;
    std::vector<Arrival> _arrivals;
    std::optional<FrameError> _error;
};

} // namespace vthost::keeper
