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

#include <crispy/debuglog.h>

#include <utility>
#include <vector>

using std::tuple;
using std::min;
using std::max;
using std::vector;

namespace text {

namespace {
    auto FontScaleTag = crispy::debugtag::make("font.scaling", "Logs about font's glyph scaling metrics, if required.");
}

tuple<rasterized_glyph, float> scale(rasterized_glyph const& _bitmap, crispy::ImageSize _newSize)
{
    assert(_bitmap.format == bitmap_format::rgba);
    // assert(_bitmap.width <= _width);
    // assert(_bitmap.height <= _height);

    auto const ratioX = float(*_bitmap.size.width) / float(*_newSize.width);
    auto const ratioY = float(*_bitmap.size.height) / float(*_newSize.height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = int(ceilf(ratio));

    vector<uint8_t> dest;
    dest.resize(*_newSize.height * *_newSize.width * 4);

    debuglog(FontScaleTag).write("scaling from {} to {}, ratio {}x{} ({}), factor {}",
                                 _bitmap.size, _newSize, ratioX, ratioY, ratio, factor);

    uint8_t* d = dest.data();
    for (int i = 0, sr = 0; i < *_newSize.height; i++, sr += factor)
    {
        for (int j = 0, sc = 0; j < *_newSize.width; j++, sc += factor, d += 4)
        {
            // calculate area average
            unsigned int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int y = sr; y < min(sr + factor, _bitmap.size.height.as<int>()); y++)
            {
                uint8_t const* p = _bitmap.bitmap.data() + (y * *_bitmap.size.width * 4) + sc * 4;
                for (int x = sc; x < min(sc + factor, _bitmap.size.width.as<int>()); x++, count++)
                {
                    b += *(p++);
                    g += *(p++);
                    r += *(p++);
                    a += *(p++);
                }
            }

            if (count)
            {
                d[0] = b / count;
                d[1] = g / count;
                d[2] = r / count;
                d[3] = a / count;
            }
        }
    }

    auto output = rasterized_glyph{};
    output.format = _bitmap.format;
    output.size = _newSize;
    output.position = _bitmap.position; // TODO Actually, left/top position should be adjusted
    output.bitmap = move(dest);

    return {output, factor};
}

}
