// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ClipboardProtocol.hpp
/// @brief The bytes of the two terminal clipboard protocols, and the terminal's answers to them.
///
/// OSC 52 is the protocol nearly every terminal speaks: plain text, written and never confirmed.
/// OSC 5522 is kitty's (https://sw.kovidgoyal.net/kitty/clipboard/): any MIME type, data in
/// chunks, and a status reply that says whether the copy happened. This file composes the
/// sequences and reads the replies; it does no I/O. @c ClipboardWriter is what sends them.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace core::tui
{

/// @brief Which selection a copy replaces.
enum class ClipboardTarget : std::uint8_t
{
    Clipboard, ///< The system clipboard (OSC 52 `c`, OSC 5522 without `loc`).
    Primary,   ///< The X11/Wayland primary selection (OSC 52 `p`, OSC 5522 `loc=primary`).
};

/// @brief The protocol that carried a copy.
enum class ClipboardTransport : std::uint8_t
{
    Osc5522, ///< kitty's clipboard protocol; the terminal confirmed the copy.
    Osc52,   ///< The classic protocol; the terminal does not confirm anything.
};

/// @brief Why a copy to the clipboard failed.
enum class ClipboardWriteError : std::uint8_t
{
    NoTerminal,          ///< There is no controlling terminal to send the sequence to.
    UnsupportedMimeType, ///< The terminal speaks OSC 52 only, which carries plain text only.
    PermissionDenied,    ///< The terminal or its user refused (OSC 5522 `EPERM`).
    TooLarge,            ///< More data than the terminal accepts (OSC 5522 `EFBIG`).
    Busy,                ///< The clipboard is temporarily unavailable (OSC 5522 `EBUSY`).
    InvalidData,         ///< The terminal rejected the packets (OSC 5522 `EINVAL`).
    IoError,             ///< Writing to the terminal failed, or it reported `EIO` or an unknown code.
    PrimaryUnavailable,  ///< The terminal has no primary selection (OSC 5522 `ENOSYS`).
    NoConfirmation,      ///< OSC 5522 was used, and no status arrived in time.
};

/// @brief The DEC private mode a terminal reports as known when it speaks OSC 5522.
constexpr int Osc5522Mode = 5522;

/// @brief The most raw bytes one OSC 5522 data packet carries, before base64 encoding.
constexpr std::size_t Osc5522ChunkSize = 4096;

/// @brief DECRQM for mode 5522: the terminal answers with a @c DecModeReport for it, if at all.
constexpr std::string_view Osc5522ModeQuery = "\033[?5522$p";

/// @brief The OSC 5522 probe: DECRQM for mode 5522, then DA1, which every terminal answers.
///
/// Terminals answer in order, so the DA1 reply ends the probe whether or not DECRQM was answered.
constexpr std::string_view Osc5522Probe = "\033[?5522$p\033[c";

/// @brief Composes the OSC 52 sequence that copies @p data.
/// @param data The bytes to copy.
/// @param target The selection to replace.
/// @return `ESC ] 52 ; c|p ; <base64> ESC \`.
[[nodiscard]] auto encodeOsc52(std::string_view data, ClipboardTarget target) -> std::string;

/// @brief Appends the OSC 52 sequence that copies @p data to @p out, encoding in place.
/// @param out The buffer to append to.
/// @param data The bytes to copy.
/// @param target The selection to replace.
void appendOsc52(std::string& out, std::string_view data, ClipboardTarget target);

/// @brief Composes the whole OSC 5522 packet sequence that copies @p data as @p mime.
///
/// A `type=write` packet, one `type=wdata` packet per chunk of at most @c Osc5522ChunkSize bytes
/// (one empty packet for empty data, so the MIME type is still declared), and the empty
/// `type=wdata` packet that ends the write.
/// @param data The bytes to copy.
/// @param mime Their MIME type.
/// @param target The selection to replace.
/// @return The packets, concatenated.
[[nodiscard]] auto encodeOsc5522Write(std::string_view data, std::string_view mime, ClipboardTarget target)
    -> std::string;

/// @brief Reads an OSC reply as the terminal's answer to an OSC 5522 write.
/// @param oscPayload The content between `ESC ]` and the string terminator.
/// @return std::nullopt when the reply is not a `type=write` status; otherwise success for `DONE`
///         and the error the status names for anything else.
[[nodiscard]] auto parseOsc5522WriteStatus(std::string_view oscPayload)
    -> std::optional<std::expected<void, ClipboardWriteError>>;

/// @brief A sentence that says what went wrong, for a message to the user.
/// @param error The error to describe.
/// @return A lower-case description without a trailing period.
[[nodiscard]] auto describe(ClipboardWriteError error) noexcept -> std::string_view;

/// @brief Tells whether @p mime is `text/plain`, with or without parameters.
///
/// That is the only type OSC 52 can carry: it has no field to name another.
/// @param mime A MIME type.
/// @return True for `text/plain` and `text/plain;...`.
[[nodiscard]] auto isPlainTextMimeType(std::string_view mime) noexcept -> bool;

} // namespace core::tui
