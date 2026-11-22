// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/PngDecoding.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <png.h>

namespace vtbackend
{

namespace
{
    /// The eight bytes every PNG file starts with (PNG specification, 5.2).
    constexpr auto PngSignature = std::to_array<uint8_t>({ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' });

    /// libpng's simplified-API control structure, freed whatever path leaves its scope: png_image_free()
    /// is a no-op on one libpng already freed after an error.
    class PngImage
    {
      public:
        PngImage() noexcept: _image { zeroed() } {}
        PngImage(PngImage const&) = delete;
        PngImage(PngImage&&) = delete;
        PngImage& operator=(PngImage const&) = delete;
        PngImage& operator=(PngImage&&) = delete;
        ~PngImage() { png_image_free(&_image); }

        [[nodiscard]] png_image& get() noexcept { return _image; }

      private:
        /// @return A structure libpng's begin_read functions accept: all zero but its version.
        [[nodiscard]] static png_image zeroed() noexcept
        {
            auto image = png_image {};
            image.version = PNG_IMAGE_VERSION;
            return image;
        }

        png_image _image;
    };
} // namespace

bool isPng(std::span<uint8_t const> data) noexcept
{
    return data.size() >= PngSignature.size()
           && std::ranges::equal(data.first(PngSignature.size()), PngSignature);
}

std::expected<DecodedPng, PngDecodeError> decodePngImage(std::span<uint8_t const> data,
                                                         PngDecodingLimits limits)
{
    if (!isPng(data))
        return std::unexpected(PngDecodeError::NotPng);

    auto png = PngImage {};
    auto& image = png.get();
    // Reads the header and every chunk up to the image data, and nothing is allocated for the pixels yet.
    if (png_image_begin_read_from_memory(&image, data.data(), data.size()) == 0)
        return std::unexpected(PngDecodeError::Corrupt);

    // The size is the file's say, so it is checked before it is believed: w * h fits 64 bits, as both
    // are 31 bits at most, but times 4 need not.
    constexpr auto BytesPerPixel = uint64_t { 4 };
    auto const pixels = uint64_t { image.width } * image.height;
    if (pixels == 0 || pixels > limits.maxDecodedBytes / BytesPerPixel)
        return std::unexpected(PngDecodeError::TooLarge);

    image.format = PNG_FORMAT_RGBA;
    // A 16-bit file without a colour-space chunk would be taken as linear light and gamma-encoded on the
    // way to 8 bits; it is sRGB as every other decoder (and every 8-bit file) takes it.
    image.flags |= PNG_IMAGE_FLAG_16BIT_sRGB;

    auto rgba = Image::Data(static_cast<size_t>(pixels * BytesPerPixel));
    if (png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr) == 0)
        return std::unexpected(PngDecodeError::Corrupt);

    return DecodedPng { .size = ImageSize { Width::cast_from(image.width), Height::cast_from(image.height) },
                        .rgba = std::move(rgba) };
}

PngDecoder::PngDecoder(PngDecodingLimits limits, ImageDecoder otherFormats):
    _limits { limits }, _otherFormats { std::move(otherFormats) }
{
}

std::optional<Image::Data> PngDecoder::operator()(ImageFormat format,
                                                  std::span<uint8_t const> data,
                                                  ImageSize& size) const
{
    if (format != ImageFormat::PNG)
        return std::nullopt;

    if (!isPng(data))
    {
        if (!_otherFormats)
            return std::nullopt;
        return _otherFormats(format, data, size);
    }

    auto decoded = decodePngImage(data, _limits);
    if (!decoded)
        return std::nullopt;
    size = decoded->size;
    return std::move(decoded->rgba);
}

} // namespace vtbackend
