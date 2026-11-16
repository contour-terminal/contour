// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Error.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace core::tui
{

/// @brief A local image, loaded, scaled and sixel-encoded, ready to embed.
struct PreparedImage
{
    std::string sixel;  ///< Sixel payload WITHOUT DCS framing; TerminalOutput::writeSixel() adds it.
    int cellWidth = 0;  ///< Rendered width in terminal cells (drives horizontal centering).
    int cellHeight = 0; ///< Rendered height in terminal cells (rows to advance past the image).
};

/// @brief Static configuration for turning an image reference into sixel.
///
/// Pure data, so tests construct it directly without a terminal.
struct ImageRenderConfig
{
    std::filesystem::path baseDir; ///< Directory of the markdown file; relative sources resolve here.
    int cellWidthPx = 8;           ///< Terminal cell width in pixels (fallback 8 when unknown).
    int cellHeightPx = 16;         ///< Terminal cell height in pixels (fallback 16 when unknown).
    int maxColumns = 0;            ///< Maximum render width in cells (0 disables fit-to-width).
};

/// @brief Abstract source of inline images, and the Sixel capability signal.
///
/// Injected into MarkdownRenderer. Keeping the capability on the same interface
/// gives a single source of truth: a provider that cannot produce sixel simply
/// reports so, and the renderer never calls prepare(). A single fake can express
/// both "not capable" and "capable but this image failed".
class ImageProvider
{
  public:
    virtual ~ImageProvider() = default;

    ImageProvider() = default;
    ImageProvider(ImageProvider const&) = delete;
    auto operator=(ImageProvider const&) -> ImageProvider& = delete;
    ImageProvider(ImageProvider&&) = delete;
    auto operator=(ImageProvider&&) -> ImageProvider& = delete;

    /// @brief Whether inline sixel images may be emitted at all.
    ///
    /// Called only when the renderer actually encounters an image, so a costly
    /// terminal probe behind this is not paid by image-free documents. When it
    /// returns false the renderer never calls prepare() and always emits alt text.
    ///
    /// @return true when the terminal can display Sixel graphics.
    [[nodiscard]] virtual auto supportsSixel() -> bool = 0;

    /// @brief Loads, scales and sixel-encodes a local image reference.
    ///
    /// @param src The image source exactly as written in the document.
    /// @param requestedWidthPx Explicit pixel width from an HTML `width=` attribute,
    ///        or std::nullopt to auto-fit to the configured maximum width.
    /// @return The prepared image, or an error. Every error means "render the alt text".
    [[nodiscard]] virtual auto prepare(std::string_view src, std::optional<int> requestedWidthPx)
        -> Result<PreparedImage> = 0;
};

/// @brief Reports whether the terminal can display Sixel graphics.
///
/// Taken as a callable rather than a plain bool so that an expensive terminal
/// probe is deferred until a document actually contains an image.
using SixelSupportFn = std::function<bool()>;

} // namespace core::tui
