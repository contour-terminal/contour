// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The @c ImageProvider that reads an image off the filesystem, which is the part of the image
/// API that needs a decoder. It is built only with `CORE_CPP_WITH_IMAGES`, where stb is; the
/// interface it implements is in `<core/tui/ImageProvider.hpp>` and is always there, because
/// @c MarkdownRenderer takes one by reference whether or not this implementation was built.

#include <core/tui/Error.hpp>
#include <core/tui/ImageProvider.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace core::tui
{

/// @brief Production ImageProvider backed by loadImage(), resizeImage() and encodeSixel().
///
/// Gates on the same isImageExtension() predicate and decodes with the same
/// loadImage() function that the `cat` builtin already uses for image files, so
/// the supported format set can never drift between the two.
class FilesystemImageProvider final: public ImageProvider
{
  public:
    /// @param config Base directory, cell metrics and maximum width.
    /// @param sixelSupported Queried at most once, on the document's first image.
    FilesystemImageProvider(ImageRenderConfig config, SixelSupportFn sixelSupported);

    [[nodiscard]] auto supportsSixel() -> bool override;

    [[nodiscard]] auto prepare(std::string_view src, std::optional<int> requestedWidthPx)
        -> Result<PreparedImage> override;

  private:
    ImageRenderConfig _config;
    SixelSupportFn _sixelSupported;
    std::optional<bool> _cachedSupport; ///< Memoized result of _sixelSupported().
};

/// @brief Whether @p src refers to a remote or non-filesystem resource.
///
/// Remote images are never fetched; they degrade to alt text.
///
/// @param src The image source as written in the document.
/// @return true for `http://`, `https://`, any scheme with `://`, `data:` and protocol-relative `//`.
[[nodiscard]] auto isRemoteImageSource(std::string_view src) -> bool;

} // namespace core::tui
