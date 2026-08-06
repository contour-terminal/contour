// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace vtbackend::kitty_clipboard
{

/// Largest amount of clipboard data one `write` transmission may accumulate, in decoded bytes.
///
/// The `wdata` stream is attacker-controlled and is only ever flushed by the empty end-of-transmission
/// chunk, so a stream that simply never sends one grows this buffer until the process is OOM-killed --
/// taking every tab and split pane in the window with it. This is the same bound
/// @c kitty_graphics::MaxChunkedPayloadSize places on the other chunked protocol; 8 MiB is far beyond
/// any plausible clipboard payload.
inline constexpr size_t MaxClipboardWriteSize = static_cast<size_t>(8 * 1024 * 1024);

/// Largest amount of clipboard data one read-reply `DATA` packet may carry, before base64 encoding.
///
/// "The terminal emulator should chunk up the data for an individual type, into chunks of size no
/// more than 4096 bytes (4096 is the size of a chunk *before* base64 encoding)." A client with a
/// bounded OSC buffer truncates or drops a single oversized packet.
inline constexpr size_t ReadChunkSize = 4096;

/// What an `OSC 5522` packet is asking for (`type=`).
enum class PacketType : uint8_t
{
    Read,       ///< `read` -- send me the clipboard, in one of these MIME types.
    Write,      ///< `write` -- I am about to send you clipboard data.
    WriteAlias, ///< `walias` -- these MIME names are aliases of that one.
    WriteData,  ///< `wdata` -- here is a chunk; an empty chunk ends the transmission.
};

/// Which clipboard a packet refers to (`loc=`).
enum class Location : uint8_t
{
    Clipboard,       ///< the default: the system clipboard.
    PrimarySelection ///< `primary` -- X11's primary selection.
};

/// One decoded `OSC 5522` packet.
///
/// `OSC 5522 ; <key>=<value>:<key>=<value>... ; <base64 payload> ST`
struct Packet
{
    PacketType type = PacketType::Read;
    Location location = Location::Clipboard;

    /// `mime=` -- the MIME type a `wdata` chunk or a `walias` mapping is for.
    std::string mimeType;

    /// `id=` -- echoed back on every response so a client can match them to its request.
    std::string id;

    /// The payload, still base64-encoded exactly as it arrived. For a `read` it is a
    /// space-separated list of acceptable MIME types; for `wdata` it is the clipboard data itself.
    std::string payload;
};

/// Why an `OSC 5522` packet could not be decoded.
enum class Error : uint8_t
{
    MalformedMetadata, ///< The metadata was not a colon-separated list of key=value pairs.
    UnknownType,       ///< `type=` named something this terminal does not implement.
};

/// Decodes one OSC 5522 packet.
///
/// @param payload the OSC body WITHOUT its leading "5522;".
[[nodiscard]] std::expected<Packet, Error> parsePacket(std::string_view payload);

/// @return the wire spelling of @p type, which every response echoes back in its `type=` key.
///
/// The protocol answers `type=<the type asked about>:status=<code>` -- the status never goes in the
/// `type=` slot. @see the reply shapes in kitty's docs/clipboard.rst.
[[nodiscard]] std::string_view typeName(PacketType type) noexcept;

/// @return whether @p mimeType is one this terminal can actually put on, or take off, the clipboard.
///
/// Contour's clipboard is text, so anything that is not plain text has to be refused rather than
/// accepted and silently dropped -- an application that is told its image was stored, and then reads
/// back text, is worse off than one that was told no.
[[nodiscard]] bool isSupportedMimeType(std::string_view mimeType) noexcept;

} // namespace vtbackend::kitty_clipboard
