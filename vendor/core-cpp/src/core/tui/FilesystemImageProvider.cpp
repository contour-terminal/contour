// SPDX-License-Identifier: Apache-2.0
#include <core/tui/FilesystemImageProvider.hpp>

#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/tui/ImageLoader.hpp>
#include <core/tui/Sixel.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace core::tui
{

namespace
{
    /// @brief Number of colors in the quantized sixel palette.
    constexpr auto SixelPaletteSize = 256;

    /// @brief Fallback cell metrics used when the terminal reports no pixel size.
    constexpr auto FallbackCellWidthPx = 8;
    constexpr auto FallbackCellHeightPx = 16;

    /// @brief Divides @p value by @p divisor, rounding up. Both must be positive.
    [[nodiscard]] auto ceilDiv(int value, int divisor) noexcept -> int
    {
        return (value + divisor - 1) / divisor;
    }
} // namespace

auto isRemoteImageSource(std::string_view src) -> bool
{
    return src.contains("://") || src.starts_with("data:") || src.starts_with("//");
}

FilesystemImageProvider::FilesystemImageProvider(ImageRenderConfig config, SixelSupportFn sixelSupported):
    _config(std::move(config)), _sixelSupported(std::move(sixelSupported))
{
}

auto FilesystemImageProvider::supportsSixel() -> bool
{
    if (!_cachedSupport.has_value())
        _cachedSupport = _sixelSupported && _sixelSupported();
    return *_cachedSupport;
}

auto FilesystemImageProvider::prepare(std::string_view src, std::optional<int> requestedWidthPx)
    -> Result<PreparedImage>
{
    if (src.empty())
        return std::unexpected(std::string("empty image source"));

    if (isRemoteImageSource(src))
        return std::unexpected(std::format("remote image not supported: {}", src));

    // The source is UTF-8, as markdown is, and goes back out as UTF-8 -- to loadImage() and to the
    // messages. The narrow constructor and path::string() use the ANSI code page on Windows. A
    // source that is not UTF-8 names no file there, and is refused like any other.
    auto const srcPath = core::pathFromUtf8(src);
    if (!srcPath)
        return std::unexpected(std::format("image source is not UTF-8: {}", core::escape(src)));
    auto const resolved = srcPath->is_absolute() ? *srcPath : _config.baseDir / *srcPath;

    // Same gate the `cat` builtin applies to image files, so the supported
    // format set is declared exactly once.
    if (!isImageExtension(core::pathToUtf8(resolved.extension())))
        return std::unexpected(std::format("unsupported image format: {}", core::pathToUtf8(resolved)));

    auto const cellWidthPx = _config.cellWidthPx > 0 ? _config.cellWidthPx : FallbackCellWidthPx;
    auto const cellHeightPx = _config.cellHeightPx > 0 ? _config.cellHeightPx : FallbackCellHeightPx;
    auto const maxWidthPx = _config.maxColumns > 0 ? _config.maxColumns * cellWidthPx : 0;

    return loadImage(core::pathToUtf8(resolved))
        .and_then([&](OwnedImage image) -> Result<OwnedImage> {
            auto targetWidthPx = 0;
            if (requestedWidthPx.has_value())
                targetWidthPx = maxWidthPx > 0 ? std::min(*requestedWidthPx, maxWidthPx) : *requestedWidthPx;
            else if (maxWidthPx > 0 && image.width > maxWidthPx)
                targetWidthPx = maxWidthPx;

            // Never upscale, and skip the resize when nothing would change.
            if (targetWidthPx <= 0 || targetWidthPx >= image.width)
                return image;

            return resizeImage(image, targetWidthPx, 0);
        })
        .and_then([&](OwnedImage const& image) -> Result<PreparedImage> {
            auto const data =
                ImageData { .pixels = image.pixels, .width = image.width, .height = image.height };
            return encodeSixel(data, SixelPaletteSize).transform([&](std::string sixel) {
                return PreparedImage { .sixel = std::move(sixel),
                                       .cellWidth = ceilDiv(image.width, cellWidthPx),
                                       .cellHeight = ceilDiv(image.height, cellHeightPx) };
            });
        });
}

} // namespace core::tui
