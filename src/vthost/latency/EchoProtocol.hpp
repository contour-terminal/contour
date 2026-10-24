// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The contract between the echo child and every side that measures it.
///
/// The child prints a prompt and echoes each byte it reads; a measuring side types `keyFor(i)` and
/// waits for a frame that draws the cursor at `expectedCursorColumn(i)`. The column — not the
/// character — is what is detected, as it is the one thing a frame shows without rasterizing text.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vthost::latency
{

/// What the echo child prints before it reads the first byte, and again after each wrap.
inline constexpr std::string_view PromptText = "> ";
/// The width of PromptText, which is the cursor column (0-based) right after it.
inline constexpr int PromptColumns = 2;
/// How many echoed bytes fit on a line before the child wraps to a fresh prompt.
inline constexpr int EchoWrapColumns = 60;
/// The byte (^D) that ends the echo child.
inline constexpr char QuitByte = '\x04';

/// What a flood child prints before it ends, followed by the number of bytes it wrote: how the measuring
/// side learns the flood is over, and how much of it there was.
inline constexpr std::string_view FloodEndMarker = "flood-bytes ";

/// @param keyIndex The 0-based index of the key pressed.
/// @return The letter that key types: 'a' to 'z', then around again.
[[nodiscard]] constexpr char keyFor(std::size_t keyIndex) noexcept
{
    return static_cast<char>('a' + (keyIndex % 26));
}

/// @param keyIndex The 0-based index of the key pressed.
/// @return The 0-based cursor column once that key has been echoed.
[[nodiscard]] constexpr int expectedCursorColumn(std::size_t keyIndex) noexcept
{
    return PromptColumns + static_cast<int>(keyIndex % EchoWrapColumns) + 1;
}

/// What the echo child tracks between bytes.
struct EchoState
{
    int column = PromptColumns; ///< The 0-based column the cursor is at.
};

/// What the echo child does after a byte.
enum class EchoAction : uint8_t
{
    Continue = 0,
    Quit = 1,
};

/// The echo child's answer to one byte.
struct EchoReply
{
    EchoAction action = EchoAction::Continue;
    std::string bytes; ///< What to write to the terminal.
};

/// Answers one byte read from the terminal.
/// @param state The child's state; advanced.
/// @param byte The byte just read.
/// @return What to write, and whether to go on.
[[nodiscard]] inline EchoReply echoReply(EchoState& state, char byte)
{
    if (byte == QuitByte)
        return EchoReply { .action = EchoAction::Quit, .bytes = {} };
    auto reply = EchoReply {};
    if (state.column == PromptColumns + EchoWrapColumns)
    {
        reply.bytes = "\r\n";
        reply.bytes += PromptText;
        state.column = PromptColumns;
    }
    reply.bytes += byte;
    ++state.column;
    return reply;
}

/// What closes a flood child's byte count, so that a screen caught mid-line never passes for a count.
inline constexpr std::string_view FloodEndTerminator = " done";

/// @param output What a flood child left on the screen.
/// @return The byte count after FloodEndMarker; nullopt while the line is not complete (yet) or its number
///         is not.
[[nodiscard]] inline std::optional<std::size_t> floodedBytes(std::string_view output)
{
    auto const at = output.rfind(FloodEndMarker);
    if (at == std::string_view::npos)
        return std::nullopt;
    auto const rest = output.substr(at + FloodEndMarker.size());
    auto const digitCount = rest.find_first_not_of("0123456789");
    if (digitCount == 0 || digitCount == std::string_view::npos
        || !rest.substr(digitCount).starts_with(FloodEndTerminator))
        return std::nullopt;
    // Digit by digit: a count has no more than 19 of them, so it cannot overflow.
    constexpr auto MaxDigits = std::size_t { 19 };
    if (digitCount > MaxDigits)
        return std::nullopt;
    auto value = std::size_t { 0 };
    for (auto const digit: rest.substr(0, digitCount))
        value = (value * 10) + static_cast<std::size_t>(digit - '0');
    return value;
}

} // namespace vthost::latency
