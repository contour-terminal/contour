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

tuple<rasterized_glyph, float> scale(rasterized_glyph const& _bitmap, crispy::ImageSize _newSize)
{
    assert(_bitmap.format == bitmap_format::rgba);
    // assert(_bitmap.width <= _width);
    // assert(_bitmap.height <= _height);

    auto const ratioX = unbox<double>(_bitmap.bitmapSize.width) / unbox<double>(_newSize.width);
    auto const ratioY = unbox<double>(_bitmap.bitmapSize.height) / unbox<double>(_newSize.height);
    auto const ratio = min(ratioX, ratioY);
    auto const factor = static_cast<unsigned>(ceil(ratio));

    vector<uint8_t> dest;
    dest.resize(_newSize.area() * 4 /* color component count, RGBA */);

    RasterizerLog()("scaling from {} to {}, ratio {}x{} ({}), factor {}",
                    _bitmap.bitmapSize,
                    _newSize,
                    ratioX,
                    ratioY,
                    ratio,
                    factor);

    uint8_t* d = dest.data();
    for (unsigned i = 0, sr = 0; i < *_newSize.height; i++, sr += factor)
    {
        for (unsigned j = 0, sc = 0; j < *_newSize.width; j++, sc += factor, d += 4)
        {
            // calculate area average
            unsigned int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (unsigned y = sr; y < min(sr + factor, _bitmap.bitmapSize.height.as<unsigned>()); y++)
            {
                uint8_t const* p = _bitmap.bitmap.data() + (y * *_bitmap.bitmapSize.width * 4) + sc * 4;
                for (unsigned x = sc; x < min(sc + factor, _bitmap.bitmapSize.width.as<unsigned>());
                     x++, count++)
                {
                    b += *(p++);
                    g += *(p++);
                    r += *(p++);
                    a += *(p++);
                }
            }

            if (count)
            {
                d[0] = static_cast<uint8_t>(b / count);
                d[1] = static_cast<uint8_t>(g / count);
                d[2] = static_cast<uint8_t>(r / count);
                d[3] = static_cast<uint8_t>(a / count);
            }
        }
    }

    auto output = rasterized_glyph {};
    output.format = _bitmap.format;
    output.bitmapSize = _newSize;
    output.position = _bitmap.position; // TODO Actually, left/top position should be adjusted
    output.bitmap = move(dest);
    output.position.x /= static_cast<int>(factor);
    output.position.y /= static_cast<int>(factor);

    return { output, factor };
}

} // namespace text
