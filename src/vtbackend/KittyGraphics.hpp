// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace vtbackend::kitty_graphics
{

/// Largest payload one chunked transmission (`m=1`) may accumulate, in base64 bytes.
///
/// The chunk stream is attacker-controlled: every byte a remote host writes to the terminal ends up
/// here, and a stream that never sends its `m=0` terminator would otherwise grow until the process
/// is killed -- taking every pane in the window with it. 32 MiB of base64 decodes to roughly a
/// 2500x2500 RGBA image, far beyond what a terminal is asked to show.
inline constexpr size_t MaxChunkedPayloadSize = static_cast<size_t>(32 * 1024 * 1024);

/// Largest total size, in decoded bytes, of the images held for later display.
///
/// Bounds the other half of the same attack: ids are 32-bit, so without a quota an application can
/// park 2^32 distinct images in the terminal. A transmission that would exceed this is refused with
/// `ENOSPC` rather than silently evicting an image the application still intends to place.
inline constexpr size_t MaxStoredImageBytes = static_cast<size_t>(128 * 1024 * 1024);

/// What the application is asking the terminal to do.
///
/// Kitty spells this `a=`. The letters are the wire values and are not ours to choose.
enum class Action : uint8_t
{
    Query = 'q',              ///< Validate the command and answer, without storing anything.
    Transmit = 't',           ///< Store the image for later display.
    TransmitAndDisplay = 'T', ///< Store it and display it at the cursor.
    Put = 'p',                ///< Display an already-stored image.
    Delete = 'd',             ///< Delete images and/or placements.
    Frame = 'f',              ///< Animation frame transmission.
    Animate = 'a',            ///< Animation control.
    Compose = 'c',            ///< Compose animation frames.
};

/// How the pixel data is laid out (`f=`).
enum class Format : uint8_t
{
    Rgb = 24,  ///< 3 bytes per pixel.
    Rgba = 32, ///< 4 bytes per pixel.
    Png = 100, ///< A whole PNG file, decoded by the terminal.
};

/// Where the pixel data comes from (`t=`).
enum class Medium : uint8_t
{
    Direct = 'd',        ///< Base64 in the escape sequence itself.
    File = 'f',          ///< A path to a regular file.
    TemporaryFile = 't', ///< A path to a file the terminal deletes after reading.
    SharedMemory = 's',  ///< A POSIX shared-memory object name.
};

/// Payload compression (`o=`).
enum class Compression : uint8_t
{
    None,
    ZlibDeflate, ///< `o=z`
};

/// One decoded kitty graphics command.
///
/// Field names follow the protocol's own vocabulary rather than the single letters it puts on the
/// wire, so that a reader who does not have the specification open can still follow the code.
struct Command
{
    Action action = Action::Transmit;
    Format format = Format::Rgba;
    Medium medium = Medium::Direct;
    Compression compression = Compression::None;

    /// Client-chosen image identity (`i=`). Zero means "unset".
    uint32_t imageId = 0;

    /// Alternative identity (`I=`) for clients that would rather the terminal assign the id.
    uint32_t imageNumber = 0;

    /// Distinguishes several placements of the same image (`p=`).
    uint32_t placementId = 0;

    /// Source pixel dimensions (`s=`, `v=`). Required for Rgb/Rgba, ignored for Png.
    uint32_t pixelWidth = 0;
    uint32_t pixelHeight = 0;

    /// Region of the source image to display (`x=`, `y=`, `w=`, `h=`). Zero width/height means all.
    uint32_t sourceX = 0;
    uint32_t sourceY = 0;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;

    /// Display size in cells (`c=`, `r=`). Zero means "derive from the pixel size".
    uint32_t columns = 0;
    uint32_t rows = 0;

    /// Stacking order against the text (`z=`). Negative means behind it.
    int32_t zIndex = 0;

    /// Whether the cursor should move to just past the image (`C=`; 1 means do NOT move it).
    bool doNotMoveCursor = false;

    /// More chunks of this image follow (`m=1`).
    bool moreChunksFollow = false;

    /// Suppress success responses (`q=1`) or all responses (`q=2`).
    bool quietOnSuccess = false;
    bool quietAlways = false;

    /// What a Delete action targets (`d=`), e.g. 'a' for all, 'i' by id, 'n' by number.
    /// An upper-case letter additionally frees the stored image data, not just the placement.
    char deleteTarget = 'a';

    /// The payload, still base64-encoded exactly as it arrived.
    std::string payload;
};

/// Why a command could not be decoded. The strings are the ones kitty puts on the wire, so that an
/// application matching on them behaves the same against Contour.
enum class Error : uint8_t
{
    InvalidControlData, ///< A key=value pair was malformed.
    InvalidFormat,      ///< `f=` named a format we do not implement.
    InvalidMedium,      ///< `t=` named a medium we do not implement.
    InvalidAction,      ///< `a=` named an action we do not implement.
    MissingDimensions,  ///< A raw pixel transmission gave no width/height.
};

/// @return the wire spelling of @p error, e.g. "EINVAL".
[[nodiscard]] std::string_view errorCode(Error error) noexcept;

/// Decodes the control data and payload of one kitty graphics command.
///
/// @param apcPayload the APC body WITHOUT its leading 'G', i.e. `<control data>[;<payload>]`.
/// @return the decoded command, or the reason it could not be decoded.
[[nodiscard]] std::expected<Command, Error> parseCommand(std::string_view apcPayload);

/// @return the number of bytes one pixel occupies in @p format, or 0 when that is not a fixed number.
[[nodiscard]] constexpr unsigned bytesPerPixel(Format format) noexcept
{
    switch (format)
    {
        case Format::Rgb: return 3;
        case Format::Rgba: return 4;
        case Format::Png: return 0;
    }
    return 0;
}

} // namespace vtbackend::kitty_graphics
