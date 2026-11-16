// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Error.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// @brief Owned RGBA image data.
struct OwnedImage
{
    std::vector<std::uint8_t> pixels; ///< RGBA pixel data (4 bytes per pixel).
    int width = 0;                    ///< Image width in pixels.
    int height = 0;                   ///< Image height in pixels.
};

/// @brief Loads an image file from disk into RGBA pixel data.
///
/// Supports PNG, JPEG, GIF, BMP, TGA, PSD, HDR, PNM, PGM, PPM formats.
///
/// @param filePath Path to the image file, in UTF-8 (core::pathToUtf8()), on Windows too.
/// @return Owned image data on success, or an error string.
[[nodiscard]] auto loadImage(std::string_view filePath) -> Result<OwnedImage>;

/// @brief Resizes an image to target dimensions, preserving aspect ratio.
///
/// If one target dimension is 0, it is computed from the aspect ratio of the source image.
/// Both dimensions must not be 0 simultaneously.
///
/// @param image Source image to resize.
/// @param targetWidth Desired width (0 to auto-compute from height).
/// @param targetHeight Desired height (0 to auto-compute from width).
/// @return Resized image on success, or an error string.
[[nodiscard]] auto resizeImage(OwnedImage const& image, int targetWidth, int targetHeight)
    -> Result<OwnedImage>;

/// @brief Loads an image from raw bytes in memory into RGBA pixel data.
///
/// Uses stbi_load_from_memory(). Supports the same formats as loadImage().
///
/// @param data Raw image bytes (PNG, JPEG, GIF, BMP, etc.).
/// @return Owned image data on success, or an error string.
[[nodiscard]] auto loadImageFromMemory(std::span<std::uint8_t const> data) -> Result<OwnedImage>;

/// @brief Detects the image media type from raw bytes by checking magic byte signatures.
///
/// Supports PNG, JPEG, GIF, BMP, and WebP formats.
///
/// @param data Raw bytes to inspect.
/// @return MIME type string (e.g. "image/png"), or empty string if not recognized.
[[nodiscard]] auto detectImageMediaType(std::span<std::uint8_t const> data) -> std::string;

/// @brief Checks if a file extension corresponds to a supported image format.
///
/// Supported: .png .jpg .jpeg .gif .bmp .tga .psd .hdr .pnm .pgm .ppm
///
/// @param ext File extension including the dot (e.g., ".png").
/// @return true if the extension is a recognized image format.
[[nodiscard]] auto isImageExtension(std::string_view ext) -> bool;

/// @brief Image data read from the system clipboard.
struct ClipboardImage
{
    std::vector<std::uint8_t> data; ///< Raw image bytes (PNG, JPEG, etc.).
    std::string mediaType;          ///< MIME type (e.g. "image/png").
};

/// @brief Reads image data from the system clipboard using platform-specific tools.
///
/// On Wayland, uses wl-paste. On X11, uses xclip. Returns nullopt if no image
/// data is available in the clipboard or the required tool is not installed.
///
/// @return Clipboard image data on success, or nullopt.
[[nodiscard]] auto readClipboardImage() -> std::optional<ClipboardImage>;

} // namespace core::tui
