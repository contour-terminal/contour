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
#include <vtrasterizer/utils.h>

#include <range/v3/view/iota.hpp>

#include <algorithm> // max?
#include <cassert>

namespace terminal::rasterizer
{

using namespace std;

vector<uint8_t> downsampleRGBA(vector<uint8_t> const& bitmap, ImageSize size, ImageSize newSize)
{
    assert(size.width >= newSize.width);
    assert(size.height >= newSize.height);

    auto const ratioX = unbox<double>(size.width) / unbox<double>(newSize.width);
    auto const ratioY = unbox<double>(size.height) / unbox<double>(newSize.height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = static_cast<unsigned>(ceil(ratio));

    std::vector<uint8_t> dest;
    dest.resize(*newSize.height * *newSize.width * 4);

    // RasterizerLog()("scaling from {} to {}, ratio {}x{} ({}), factor {}",
    //                 size, newSize, ratioX, ratioY, ratio, factor);

    uint8_t* d = dest.data();
    // TODO: use iota
    for (unsigned i = 0, sr = 0; i < *newSize.height; i++, sr += factor)
    {
        for (unsigned j = 0, sc = 0; j < *newSize.width; j++, sc += factor, d += 4)
        {
            // calculate area average
            unsigned int r = 0;
            unsigned int g = 0;
            unsigned int b = 0;
            unsigned int a = 0;
            unsigned int count = 0;
            for (unsigned y = sr; y < min(sr + factor, size.height.as<unsigned>()); y++)
            {
                uint8_t const* p = bitmap.data() + (y * *size.width * 4) + sc * 4;
                for (unsigned x = sc; x < min(sc + factor, size.width.as<unsigned>()); x++, count++)
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

vector<uint8_t> downsample(vector<uint8_t> const& bitmap,
                           uint8_t numComponents,
                           ImageSize size,
                           ImageSize newSize)
{
    assert(size.width >= newSize.width);
    assert(size.height >= newSize.height);

    auto const ratioX = unbox<double>(size.width) / unbox<double>(newSize.width);
    auto const ratioY = unbox<double>(size.height) / unbox<double>(newSize.height);
    auto const ratio = max(ratioX, ratioY);
    auto const factor = static_cast<unsigned>(ceil(ratio));

    std::vector<uint8_t> dest(*newSize.width * *newSize.height * numComponents, 0);

    rasterizerLog()("downsample from {} to {}, ratio {}x{} ({}), factor {}",
                    size,
                    newSize,
                    ratioX,
                    ratioY,
                    ratio,
                    factor);

    uint8_t* d = dest.data();
    // TODO: use iota
    for (unsigned i = 0, sr = 0; i < *newSize.height; i++, sr += factor)
    {
        for (unsigned j = 0, sc = 0; j < *newSize.width; j++, sc += factor, d += numComponents)
        {
            // calculate area average
            vector<unsigned> values;
            values.resize(numComponents);
            unsigned count = 0; // number of pixels being averaged
            for (auto y = sr; y < min(sr + factor, size.height.as<unsigned>()); y++)
            {
                uint8_t const* p = bitmap.data() + (y * *size.width * numComponents) + sc * numComponents;
                for (auto x = sc; x < min(sc + factor, size.width.as<unsigned>()); x++, count++)
                    for (auto const k: ::ranges::views::iota(0u, numComponents))
                        values.at(k) += *(p++);
            }

            if (count)
            {
                for (auto const i: ::ranges::views::iota(0u, values.size()))
                    d[i] = static_cast<uint8_t>(values[i] / count);
            }
        }
    }

    return dest;
}

vector<uint8_t> downsample(vector<uint8_t> const& sourceBitmap, ImageSize targetSize, uint8_t factor)
{
    vector<uint8_t> targetBitmap(*targetSize.width * *targetSize.height, 0);

    auto const sourceWidth = factor * *targetSize.width;
    auto const averageIntensityInSrc = [&](unsigned destX, unsigned destY) -> unsigned {
        auto sourceY = destY * factor;
        auto sourceX = destX * factor;
        auto total = 0u;
        for (auto const y: ::ranges::views::iota(sourceY, sourceY + factor))
        {
            auto const offset = sourceWidth * y;
            for (auto const x: ::ranges::views::iota(sourceX, sourceX + factor))
                total += sourceBitmap[offset + x];
        }
        return unsigned(double(total) / double(factor * factor));
    };

    for (auto const y: ::ranges::views::iota(0u, *targetSize.height))
    {
        auto const offset = *targetSize.width * y;
        for (auto const x: ::ranges::views::iota(0u, *targetSize.width))
            targetBitmap[offset + x] =
                (uint8_t) min(255u, unsigned(targetBitmap[offset + x]) + averageIntensityInSrc(x, y));
    }

    return targetBitmap;
}

} // namespace terminal::rasterizer
