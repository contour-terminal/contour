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
#include <terminal_renderer/utils.h>

#include <range/v3/view/iota.hpp>

#include <algorithm> // max?

#include <cassert>

namespace terminal::renderer {

using namespace std;
using ranges::views::iota;

vector<uint8_t> downsampleRGBA(vector<uint8_t> const& _bitmap,
                               ImageSize _size,
                               ImageSize _newSize)
{
    assert(_size.width >= _newSize.width);
    assert(_size.height >= _newSize.height);

    auto const ratioX = unbox<double>(_size.width) / unbox<double>(_newSize.width);
    auto const ratioY = unbox<double>(_size.height) / unbox<double>(_newSize.height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = static_cast<unsigned>(ceil(ratio));

    std::vector<uint8_t> dest;
    dest.resize(*_newSize.height * *_newSize.width * 4);

    // LOGSTORE(RasterizerLog)("scaling from {} to {}, ratio {}x{} ({}), factor {}",
    //                         _size, _newSize, ratioX, ratioY, ratio, factor);

    uint8_t* d = dest.data();
    // TODO: use iota
    for (unsigned i = 0, sr = 0; i < *_newSize.height; i++, sr += factor)
    {
        for (unsigned j = 0, sc = 0; j < *_newSize.width; j++, sc += factor, d += 4)
        {
            // calculate area average
            unsigned int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (unsigned y = sr; y < min(sr + factor, _size.height.as<unsigned>()); y++)
            {
                uint8_t const* p = _bitmap.data() + (y * *_size.width * 4) + sc * 4;
                for (unsigned x = sc; x < min(sc + factor, _size.width.as<unsigned>()); x++, count++)
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

    return dest;
}

vector<uint8_t> downsample(vector<uint8_t> const& _bitmap,
                           uint8_t _numComponents,
                           ImageSize _size,
                           ImageSize _newSize)
{
    assert(_size.width >= _newSize.width);
    assert(_size.height >= _newSize.height);

    auto const ratioX = unbox<double>(_size.width) / unbox<double>(_newSize.width);
    auto const ratioY = unbox<double>(_size.height) / unbox<double>(_newSize.height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = static_cast<unsigned>(ceil(ratio));

    std::vector<uint8_t> dest(*_newSize.width * *_newSize.height * _numComponents, 0);

    LOGSTORE(RasterizerLog)("downsample from {} to {}, ratio {}x{} ({}), factor {}",
                            _size, _newSize, ratioX, ratioY, ratio, factor);

    uint8_t* d = dest.data();
    // TODO: use iota
    for (unsigned i = 0, sr = 0; i < *_newSize.height; i++, sr += factor)
    {
        for (unsigned j = 0, sc = 0; j < *_newSize.width; j++, sc += factor, d += _numComponents)
        {
            // calculate area average
            vector<unsigned> values;
            values.resize(_numComponents);
            unsigned count = 0; // number of pixels being averaged
            for (auto y = sr; y < min(sr + factor, _size.height.as<unsigned>()); y++)
            {
                uint8_t const* p = _bitmap.data() + (y * *_size.width * _numComponents) + sc * _numComponents;
                for (auto x = sc; x < min(sc + factor, _size.width.as<unsigned>()); x++, count++)
                    for (auto const k: iota(0u, _numComponents))
                        values.at(k) += *(p++);
            }

            if (count)
            {
                for (auto const i: iota(0u, values.size()))
                    d[i] = static_cast<uint8_t>(values[i] / count);
            }
        }
    }

    return dest;
}

vector<uint8_t> downsample(vector<uint8_t> const& _sourceBitmap,
                           ImageSize _targetSize,
                           uint8_t _factor)
{
    vector<uint8_t> targetBitmap(*_targetSize.width * *_targetSize.height, 0);

    auto const sourceWidth = _factor * *_targetSize.width;
    auto const average_intensity_in_src = [&](int destX, int destY) -> int {
        auto sourceY = destY * _factor;
        auto sourceX = destX * _factor;
        auto total = 0u;
        for (auto const y: iota(sourceY, sourceY + _factor))
        {
            auto const offset = sourceWidth * y;
            for (auto const x: iota(sourceX, sourceX + _factor))
                total += _sourceBitmap[offset + x];
        }
        return int(double(total) / double(_factor * _factor));
    };

    for (auto const y: iota(0u, *_targetSize.height))
    {
        auto const offset = *_targetSize.width * y;
        for (auto const x: iota(0u, *_targetSize.width))
            targetBitmap[offset + x] = min(
                255,
                targetBitmap[offset + x] + average_intensity_in_src(x, y)
            );
    }

    return targetBitmap;
}

}
