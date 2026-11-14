// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <vthost/keeper/Frame.hpp>
#include <vthost/keeper/Messages.hpp>

namespace vthost::keeper
{

/// Why a frame body is not a message.
enum class CodecError : uint8_t
{
    Truncated = 0,      ///< The body ends inside the tag or a field.
    UnknownTag,         ///< The tag names no message this reader knows; answer Unsupported, keep the link.
    Malformed,          ///< The fields are not well-formed.
    TooManyDescriptors, ///< The frame carries more descriptors than its message may.
};

/// What a receiver does with a frame that is not a message it can read.
enum class DecodeFailure : uint8_t
{
    /// A newer peer's: it answers Unsupported{tag}, closes the frame's descriptors, and keeps the link.
    Unsupported = 0,
    Broken, ///< The frame was cut short, or names no readable tag: the link is broken.
};

/// @param error Why a frame is no message.
/// @return What its receiver does with it: every failure but a body cut short (Truncated) is a message of a
///         later version (an unknown tag, an enum value beyond the table, more descriptors), answered
///         Unsupported (@see Messages.hpp).
[[nodiscard]] constexpr DecodeFailure failureOf(CodecError error) noexcept
{
    switch (error)
    {
        case CodecError::Truncated: return DecodeFailure::Broken;
        case CodecError::UnknownTag:
        case CodecError::Malformed:
        case CodecError::TooManyDescriptors: return DecodeFailure::Unsupported;
    }
    return DecodeFailure::Broken;
}

/// Encodes @p message as a frame body: its tag, then its fields.
/// @param message The message.
/// @return The body.
[[nodiscard]] std::vector<uint8_t> encode(Message const& message);

/// Decodes a frame body. Fields the reader does not know are skipped.
/// @param body What encode() wrote, or what a newer writer wrote.
/// @return The message, or why it is none.
[[nodiscard]] std::expected<Message, CodecError> decode(std::span<uint8_t const> body);

/// @param body A frame body.
/// @return Its tag, if it has a readable one, so a receiver can answer Unsupported{tag} for a message it
///         cannot decode.
[[nodiscard]] std::optional<uint32_t> peekTag(std::span<uint8_t const> body);

/// Decodes a received frame and holds its descriptors to its message's budget.
/// @param frame What the FrameAssembler produced.
/// @return The message, or why it is none; TooManyDescriptors when the frame carries more descriptors than
///         the message's MaxDescriptors. The caller closes the frame's descriptors on an error.
[[nodiscard]] std::expected<Message, CodecError> decodeFrame(Frame const& frame);

/// @param own  This side's channel version.
/// @param peer The other side's.
/// @return The version both speak: the lower one. Hello is never refused.
[[nodiscard]] constexpr uint32_t negotiate(uint32_t own, uint32_t peer) noexcept
{
    return own < peer ? own : peer;
}

} // namespace vthost::keeper
