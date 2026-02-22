// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/primitives.h>

#include <text_shaper/shaper.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <utility>
#include <vector>

using std::max;
using std::min;
using std::tuple;
using std::vector;

using std::views::iota;

namespace text
{

namespace
{

    template <std::size_t NumComponents>
    vector<uint8_t> scaleDown(vector<uint8_t> const& inputBitmap,
                              vtbackend::ImageSize inputSize,
                              vtbackend::ImageSize outputSize,
                              double ratio)
    {
        vector<uint8_t> ret(outputSize.area() * NumComponents);

        auto const wIn = unbox<size_t>(inputSize.width);
        auto const hIn = unbox<size_t>(inputSize.height);
        auto const wOut = unbox<size_t>(outputSize.width);
        auto const hOut = unbox<size_t>(outputSize.height);

        for (size_t const y: std::views::iota(size_t { 0 }, hOut))
        {
            auto const sourceRowTop = static_cast<double>(y) * ratio;
            auto const sourceRowBottom = static_cast<double>(y + 1) * ratio;
            auto const rowStart = static_cast<size_t>(std::floor(sourceRowTop));
            auto const rowEnd = min(static_cast<size_t>(std::ceil(sourceRowBottom)), hIn);

            for (size_t const x: std::views::iota(size_t { 0 }, wOut))
            {
                auto const sourceColumnLeft = static_cast<double>(x) * ratio;
                auto const sourceColumnRight = static_cast<double>(x + 1) * ratio;
                auto const colStart = static_cast<size_t>(std::floor(sourceColumnLeft));
                auto const colEnd = min(static_cast<size_t>(std::ceil(sourceColumnRight)), wIn);

                std::array<double, NumComponents> sums {};
                double totalWeight = 0.0;

                for (size_t const i: std::views::iota(rowStart, rowEnd))
                {
                    double const weightY = min(static_cast<double>(i + 1), sourceRowBottom)
                                           - max(static_cast<double>(i), sourceRowTop);

                    auto const* const rowPtr = inputBitmap.data() + (i * wIn * NumComponents);

                    for (size_t const j: std::views::iota(colStart, colEnd))
                    {
                        double const weightX = min(static_cast<double>(j + 1), sourceColumnRight)
                                               - max(static_cast<double>(j), sourceColumnLeft);
                        double const weight = weightY * weightX;

                        auto const* const pixelPtr = rowPtr + (j * NumComponents);
                        for (size_t const c: std::views::iota(size_t { 0 }, NumComponents))
                        {
                            sums[c] += static_cast<double>(pixelPtr[c]) * weight;
                        }
                        totalWeight += weight;
                    }
                }

                auto* const destPtr = ret.data() + (y * wOut * NumComponents) + (x * NumComponents);
                if (totalWeight > 0.0)
                {
                    for (size_t c = 0; c < NumComponents; ++c)
                    {
                        destPtr[c] = static_cast<uint8_t>(std::clamp(sums[c] / totalWeight, 0.0, 255.0));
                    }
                }
            }
        }
        return ret;
    }

} // namespace

tuple<rasterized_glyph, float> scale(rasterized_glyph const& bitmap, vtbackend::ImageSize boundingBox)
{
    // NB: We're only supporting down-scaling.
    assert(bitmap.bitmapSize.width >= boundingBox.width || bitmap.bitmapSize.height >= boundingBox.height);

    auto const ratioX = unbox<double>(bitmap.bitmapSize.width) / unbox<double>(boundingBox.width);
    auto const ratioY = unbox<double>(bitmap.bitmapSize.height) / unbox<double>(boundingBox.height);
    auto const ratio = max(ratioX, ratioY);

    // Adjust new image size to respect ratio.
    auto const newSize = vtbackend::ImageSize {
        vtbackend::Width::cast_from(unbox<double>(bitmap.bitmapSize.width) / ratio),
        vtbackend::Height::cast_from(unbox<double>(bitmap.bitmapSize.height) / ratio)
    };

    rasterizerLog()("scaling {} from {} to {}, ratio {}x{} ({})",
                    bitmap.format,
                    bitmap.bitmapSize,
                    newSize,
                    ratioX,
                    ratioY,
                    ratio);

    vector<uint8_t> dest {};
    switch (bitmap.format)
    {
        case bitmap_format::rgba:
        case bitmap_format::outlined:
            dest = scaleDown<4>(bitmap.bitmap, bitmap.bitmapSize, newSize, ratio);
            break;
        case bitmap_format::rgb: //
            dest = scaleDown<3>(bitmap.bitmap, bitmap.bitmapSize, newSize, ratio);
            break;
        case bitmap_format::alpha_mask:
            dest = scaleDown<1>(bitmap.bitmap, bitmap.bitmapSize, newSize, ratio);
            break;
    }

    auto output = rasterized_glyph {};
    output.format = bitmap.format;
    output.bitmapSize = newSize;
    output.position = bitmap.position;
    output.bitmap = std::move(dest);

    // Horizontally center the glyph and position it slightly below vertical center for visual balance.
    output.position.x = unbox<int>(boundingBox.width - output.bitmapSize.width) / 2;
    output.position.y =
        unbox<int>(output.bitmapSize.height) + unbox<int>(boundingBox.height - output.bitmapSize.height) / 4;

    return { output, static_cast<float>(ratio) };
}

} // namespace text
