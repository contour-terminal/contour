// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/primitives.h>

#include <text_shaper/shaper.h>

#include <crispy/logstore.h>

#include <range/v3/view/iota.hpp>

#include <utility>
#include <vector>

using std::max;
using std::min;
using std::tuple;
using std::vector;

using ranges::views::iota;

namespace text
{

namespace
{
    template <std::size_t NumComponents>
    constexpr void scaleDownExplicit(vector<uint8_t> const& inputBitmap,
                                     vtbackend::ImageSize inputSize,
                                     vtbackend::ImageSize outputSize,
                                     size_t factor,
                                     vector<uint8_t>& outputBitmap) noexcept
    {
        outputBitmap.resize(outputSize.area() * NumComponents);

        auto index = [&](size_t i, size_t j, size_t component) {
            return 4 * i * unbox(outputSize.width) + j * 4 + component;
        };

        for (auto const i: ::ranges::views::iota(size_t { 0 }, unbox(outputSize.height)))
        {
            for (auto const j: ::ranges::views::iota(size_t { 0 }, unbox(outputSize.width)))
            {
                //  calculate area average
                std::array<unsigned int, NumComponents> components { {} };
                unsigned int count = 0;
                for (auto const y: ::ranges::views::iota(
                         i * factor, min((i + 1) * factor, unbox<size_t>(inputSize.height))))
                {
                    uint8_t const* p =
                        inputBitmap.data() + (y * unbox<size_t>(inputSize.width) * 4) + (j * factor * 4);
                    for ([[maybe_unused]] auto const _: ::ranges::views::iota(
                             j * factor, min((j + 1) * factor, unbox<size_t>(inputSize.width))))
                    {
                        ++count;
                        for (auto const componentIndex: ::ranges::views::iota(size_t { 0 }, NumComponents))
                            components[componentIndex] += *(p++);
                    }
                }

                if (count)
                {
                    for (auto const componentIndex: ::ranges::views::iota(size_t { 0 }, NumComponents))
                        outputBitmap[index(i, j, componentIndex)] =
                            static_cast<uint8_t>(components[componentIndex] / count);
                }
            }
        }
    }

} // namespace

tuple<rasterized_glyph, float> scale(rasterized_glyph const& bitmap, vtbackend::ImageSize boundingBox)
{
    // NB: We're only supporting down-scaling.
    assert(bitmap.bitmapSize.width >= boundingBox.width || bitmap.bitmapSize.height >= boundingBox.height);

    auto const ratioX = unbox<double>(bitmap.bitmapSize.width) / unbox<double>(boundingBox.width);
    auto const ratioY = unbox<double>(bitmap.bitmapSize.height) / unbox<double>(boundingBox.height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = static_cast<unsigned>(ceil(ratio));

    // Adjust new image size to respect ratio.
    auto const newSize = vtbackend::ImageSize {
        vtbackend::Width::cast_from(unbox<double>(bitmap.bitmapSize.width) / ratio),
        vtbackend::Height::cast_from(unbox<double>(bitmap.bitmapSize.height) / ratio)
    };

    rasterizerLog()("scaling {} from {} to {}, ratio {}x{} ({}), factor {}",
                    bitmap.format,
                    bitmap.bitmapSize,
                    newSize,
                    ratioX,
                    ratioY,
                    ratio,
                    factor);

    vector<uint8_t> dest {};
    switch (bitmap.format)
    {
        case bitmap_format::rgba:
            scaleDownExplicit<4>(bitmap.bitmap, bitmap.bitmapSize, newSize, factor, dest);
            break;
        case bitmap_format::rgb:
            scaleDownExplicit<3>(bitmap.bitmap, bitmap.bitmapSize, newSize, factor, dest);
            break;
        case bitmap_format::alpha_mask:
            scaleDownExplicit<1>(bitmap.bitmap, bitmap.bitmapSize, newSize, factor, dest);
            break;
    }

    auto output = rasterized_glyph {};
    output.format = bitmap.format;
    output.bitmapSize = newSize;
    output.position = bitmap.position; // TODO Actually, left/top position should be adjusted
    output.bitmap = std::move(dest);
    output.position.x = unbox<int>(boundingBox.width - output.bitmapSize.width) / 2;
    output.position.y =
        unbox<int>(output.bitmapSize.height) + unbox<int>(boundingBox.height - output.bitmapSize.height) / 4;

    return { output, factor };
}

} // namespace text
