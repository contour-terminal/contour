/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <text_shaper/shaper.h>

#include <crispy/ImageSize.h>
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
                                     crispy::ImageSize inputSize,
                                     crispy::ImageSize outputSize,
                                     size_t factor,
                                     vector<uint8_t>& outputBitmap) noexcept
    {
        outputBitmap.resize(outputSize.area() * NumComponents);
        uint8_t* d = outputBitmap.data();
        for (size_t i = 0, sr = 0; i < *outputSize.height; i++, sr += factor)
        {
            for (size_t j = 0, sc = 0; j < *outputSize.width; j++, sc += factor, d += 4)
            {
                // calculate area average
                std::array<unsigned int, NumComponents> components { {} };
                unsigned int count = 0;
                for (size_t y = sr; y < min(sr + factor, unbox<size_t>(inputSize.height)); y++)
                {
                    uint8_t const* p = inputBitmap.data() + (y * unbox<size_t>(inputSize.width) * 4) + sc * 4;
                    for (auto x = sc; x < min(sc + factor, unbox<size_t>(inputSize.width)); x++, count++)
                    {
                        for (size_t i = 0; i < NumComponents; ++i)
                            components[i] += *(p++);
                    }
                }

                if (count)
                {
                    for (size_t i = 0; i < NumComponents; ++i)
                        d[i] = static_cast<uint8_t>(components[i] / count);
                }
            }
        }
    }

} // namespace

tuple<rasterized_glyph, float> scale(rasterized_glyph const& _bitmap, crispy::ImageSize _boundingBox)
{
    // NB: We're only supporting down-scaling.
    assert(_bitmap.bitmapSize.width >= _boundingBox.width);
    assert(_bitmap.bitmapSize.height >= _boundingBox.height);

    auto const ratioX = unbox<double>(_bitmap.bitmapSize.width) / unbox<double>(_boundingBox.width);
    auto const ratioY = unbox<double>(_bitmap.bitmapSize.height) / unbox<double>(_boundingBox.height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = static_cast<unsigned>(ceil(ratio));

    // Adjust new image size to respect ratio.
    auto const newSize =
        crispy::ImageSize { crispy::Width::cast_from(unbox<double>(_bitmap.bitmapSize.width) / ratio),
                            crispy::Height::cast_from(unbox<double>(_bitmap.bitmapSize.height) / ratio) };

    RasterizerLog()("scaling {} from {} to {}, ratio {}x{} ({}), factor {}",
                    _bitmap.format,
                    _bitmap.bitmapSize,
                    newSize,
                    ratioX,
                    ratioY,
                    ratio,
                    factor);

    vector<uint8_t> dest;
    switch (_bitmap.format)
    {
        case bitmap_format::rgba:
            scaleDownExplicit<4>(_bitmap.bitmap, _bitmap.bitmapSize, newSize, factor, dest);
            break;
        case bitmap_format::rgb:
            scaleDownExplicit<3>(_bitmap.bitmap, _bitmap.bitmapSize, newSize, factor, dest);
            break;
        case bitmap_format::alpha_mask:
            scaleDownExplicit<1>(_bitmap.bitmap, _bitmap.bitmapSize, newSize, factor, dest);
            break;
    }

    // for (unsigned i = 0, sr = 0; i < *newSize.height; i++, sr += factor)
    // {
    //     for (unsigned j = 0, sc = 0; j < *newSize.width; j++, sc += factor, d += 4)
    //     {
    //         // calculate area average
    //         unsigned int r = 0, g = 0, b = 0, a = 0, count = 0;
    //         for (unsigned y = sr; y < min(sr + factor, _bitmap.bitmapSize.height.as<unsigned>()); y++)
    //         {
    //             uint8_t const* p = _bitmap.bitmap.data() + (y * *_bitmap.bitmapSize.width * 4) + sc * 4;
    //             for (unsigned x = sc; x < min(sc + factor, _bitmap.bitmapSize.width.as<unsigned>());
    //                  x++, count++)
    //             {
    //                 b += *(p++);
    //                 g += *(p++);
    //                 r += *(p++);
    //                 a += *(p++);
    //             }
    //         }
    //
    //         if (count)
    //         {
    //             d[0] = static_cast<uint8_t>(b / count);
    //             d[1] = static_cast<uint8_t>(g / count);
    //             d[2] = static_cast<uint8_t>(r / count);
    //             d[3] = static_cast<uint8_t>(a / count);
    //         }
    //     }
    // }

    auto output = rasterized_glyph {};
    output.format = _bitmap.format;
    output.bitmapSize = newSize;
    output.position = _bitmap.position; // TODO Actually, left/top position should be adjusted
    output.bitmap = move(dest);
    output.position.x = unbox<int>(_boundingBox.width - output.bitmapSize.width) / 2;
    output.position.y =
        unbox<int>(output.bitmapSize.height) + unbox<int>(_boundingBox.height - output.bitmapSize.height) / 4;

    return { output, factor };
}

} // namespace text
