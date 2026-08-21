// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>

namespace vtbackend::screenshot
{

/// The number this extension is registered under, on both the request (OSC) and the reply (PM).
inline constexpr uint16_t Code = 533;

/// Largest payload one reply message carries, before base64 encoding.
///
/// Sized like the buffer-capture extension's 4096, and for the same reason: a screenshot of a large
/// page is tens of kilobytes, and handing that to the PTY as a single write invites a stall.
///
/// It is a multiple of three, which 4096 is not, so that each chunk encodes to a whole number of
/// base64 quanta and only the last one can carry padding. That makes both ways of reassembling the
/// reply correct -- decode each chunk and concatenate, or concatenate the encoded chunks and decode
/// once -- rather than leaving the second silently producing garbage.
inline constexpr size_t MaxChunkSize = 4095;

/// The representation a screenshot is delivered in, as named by `Pf`.
///
/// The pixel formats are deliberately *reserved rather than absent*: an application that asks for one
/// is told so (@ref Status::UnsupportedFormat) instead of being met with silence, and implementing one
/// later adds a row to @ref Formats rather than changing the grammar.
enum class Format : uint8_t
{
    PlainText = 0,   ///< UTF-8 text, one LF-terminated line per row of the region.
    VTSequences = 1, ///< The text plus the SGR sequences needed to reproduce its colors.
    Sixel = 2,       ///< Reserved.
    Png = 3,         ///< Reserved.
    Rgba = 4,        ///< Reserved.
};

/// Whether this terminal can actually produce a @ref Format.
enum class Availability : uint8_t
{
    Reserved = 0,    ///< The number is spoken for, but nothing produces it yet.
    Implemented = 1, ///< The terminal produces it.
};

/// What `Ps` says about a reply message.
///
/// Every request is answered, including one that is refused: an application that writes a request and
/// then reads must never be left waiting on a reply that is not coming.
enum class Status : uint8_t
{
    EndOfData = 0,         ///< No further messages belong to this screenshot.
    Data = 1,              ///< This message carries a chunk of the screenshot.
    Denied = 2,            ///< The user, or the configuration, refused the request.
    Malformed = 3,         ///< The request could not be read.
    UnsupportedFormat = 4, ///< The format named is reserved, or is not a format at all.
    EmptyRegion = 5,       ///< The region names no cell -- its corners are inverted.
};

/// One row of the format table: what a `Pf` value means and whether it can be served.
struct FormatInfo
{
    Format format;
    std::string_view name;
    Availability availability;
};

/// Every format the protocol assigns a number to. Adding a format is adding a row here.
inline constexpr auto Formats = std::array {
    FormatInfo {
        .format = Format::PlainText, .name = "plain text", .availability = Availability::Implemented },
    FormatInfo {
        .format = Format::VTSequences, .name = "VT sequences", .availability = Availability::Implemented },
    FormatInfo { .format = Format::Sixel, .name = "sixel", .availability = Availability::Reserved },
    FormatInfo { .format = Format::Png, .name = "PNG", .availability = Availability::Reserved },
    FormatInfo { .format = Format::Rgba, .name = "RGBA", .availability = Availability::Reserved },
};

/// @param format The format to look up.
/// @return The table row describing @p format, or nullptr if no row names it.
[[nodiscard]] constexpr FormatInfo const* formatInfo(Format format) noexcept
{
    for (auto const& info: Formats)
        if (info.format == format)
            return &info;
    return nullptr;
}

/// @param format The format to test.
/// @return Whether this terminal can produce @p format.
[[nodiscard]] constexpr bool isSupported(Format format) noexcept
{
    auto const* const info = formatInfo(format);
    return info != nullptr && info->availability == Availability::Implemented;
}

/// One decoded `OSC 533` request.
///
/// `OSC 533 ; Pid ; Pt ; Pl ; Pb ; Pr ; Pf ST`
struct Request
{
    /// `Pid` -- an opaque token echoed in every reply, so an application can match a reply to the
    /// request that asked for it. Zero when the request named none.
    ///
    /// It earns its place because a reply is not necessarily prompt: the permission wall may hold a
    /// request until the user answers a dialog, by which time further requests may have been sent.
    uint32_t id = 0;

    /// The region, as zero-based offsets into the page, both corners inclusive, already defaulted
    /// and clamped. @see parseRequest().
    Rect area {};

    /// `Pf` -- the representation asked for.
    Format format = Format::PlainText;
};

/// Whether a frontend has taken ownership of a screenshot request.
///
/// Reading the screen is guarded, and the guard lives in the frontend -- it owns the configuration
/// and it is what can put a dialog in front of the user. The terminal only needs to know whether an
/// answer is coming, so that it can refuse the request itself when none is.
enum class Disposition : uint8_t
{
    Unhandled = 0, ///< No frontend will answer this; the terminal refuses it on their behalf.
    Pending = 1,   ///< The frontend will answer it, possibly only after asking the user.
};

/// What a frontend, or the user it asked, decided about a screenshot request.
enum class Decision : uint8_t
{
    Denied = 0,
    Allowed = 1,
};

/// Why a request could not be served, and which request it was.
///
/// The id travels with the status because a refusal still has to be matched to what it refuses. It is
/// zero when the payload was too malformed for even the id to be read.
struct Rejection
{
    uint32_t id = 0;
    Status status = Status::Malformed;
};

/// Decodes one OSC 533 request and resolves its region against the page.
///
/// Every parameter is optional and takes its default when omitted or empty; the coordinates take it
/// when zero as well, matching every other rectangular-area sequence (@see paramPositiveOr). The
/// coordinates are one-based and both corners are inclusive, as DEC's rectangular-area functions
/// name them, and a coordinate naming a cell beyond the page names the page's edge instead.
///
/// Unlike those functions the region is measured from the page's top-left corner and not from the
/// origin, so origin mode (DECOM) does not move it. A screenshot names the screen; what margins the
/// application happens to have set is not part of the question it is asking.
///
/// @param payload The OSC body WITHOUT its leading "533;", i.e. `Pid;Pt;Pl;Pb;Pr;Pf`.
/// @param page    The page the region is resolved against.
/// @return The decoded request, or the rejection to reply with instead.
[[nodiscard]] std::expected<Request, Rejection> parseRequest(std::string_view payload, PageSize page);

/// Receives one complete reply message, terminator included, ready to be written to the host.
using Sink = std::function<void(std::string_view)>;

/// Writes the reply carrying @p content, split into as many messages as it takes.
///
/// The content is base64-encoded. That is not decoration: a screenshot is screen content, the
/// VT-sequence format carries ESC by construction, and a raw ST anywhere in the payload would end the
/// reply early and leave the remainder to be read as input by whatever asked for it.
///
/// @param request The request being answered; its id, region and format are echoed back.
/// @param content The screenshot itself, unencoded.
/// @param sink    Invoked once per reply message.
void writeReply(Request const& request, std::string_view content, Sink const& sink);

/// Writes the single message that refuses a request.
///
/// @param requestId The `Pid` of the request being refused, echoed back.
/// @param status    Why it is refused; anything but @ref Status::Data.
/// @param sink      Invoked once.
void writeError(uint32_t requestId, Status status, Sink const& sink);

} // namespace vtbackend::screenshot
