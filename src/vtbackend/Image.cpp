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
#include <vtbackend/Image.h>

#include <algorithm>
#include <memory>

using std::copy;
using std::make_shared;
using std::min;
using std::move;
using std::ostream;
using std::shared_ptr;
using std::string;

using crispy::LRUCapacity;
using crispy::StrongHashtableSize;

namespace terminal
{

image_stats& image_stats::get()
{
    static image_stats stats {};
    return stats;
}

image::~image()
{
    --image_stats::get().instances;
    _onImageRemove(this);
}

rasterized_image::~rasterized_image()
{
    --image_stats::get().rasterized;
}

image_fragment::~image_fragment()
{
    --image_stats::get().fragments;
}

image_pool::image_pool(on_image_remove onImageRemove, image_id nextImageId):
    _nextImageId { nextImageId },
    _imageNameToImageCache { StrongHashtableSize { 1024 },
                             LRUCapacity { 100 },
                             "ImagePool name-to-image mappings" },
    _onImageRemove { std::move(onImageRemove) }
{
}

image::data rasterized_image::fragment(cell_location pos) const
{
    // TODO: respect alignment hint
    // TODO: respect resize hint

    auto const xOffset = pos.column * unbox<int>(_cellSize.width);
    auto const yOffset = pos.line * unbox<int>(_cellSize.height);
    auto const pixelOffset = cell_location { yOffset, xOffset };

    image::data fragData;
    fragData.resize(_cellSize.area() * 4); // RGBA
    auto const availableWidth =
        min(unbox<int>(_image->width()) - *pixelOffset.column, unbox<int>(_cellSize.width));
    auto const availableHeight =
        min(unbox<int>(_image->height()) - *pixelOffset.line, unbox<int>(_cellSize.height));

    // auto const availableSize = Size{availableWidth, availableHeight};
    // std::cout << fmt::format(
    //     "RasterizedImage.fragment({}): pixelOffset={}, cellSize={}/{}\n",
    //     pos,
    //     pixelOffset,
    //     _cellSize,
    //     availableSize
    // );

    // auto const fitsWidth = pixelOffset.column + _cellSize.width < _image.get().width();
    // auto const fitsHeight = pixelOffset.line + _cellSize.height < _image.get().height();
    // if (!fitsWidth || !fitsHeight)
    //     std::cout << fmt::format("ImageFragment: out of bounds{}{} ({}x{}); {}\n",
    //             fitsWidth ? "" : " (width)",
    //             fitsHeight ? "" : " (height)",
    //             availableWidth,
    //             availableHeight,
    //             *this);

    // TODO: if input format is (RGB | PNG), transform to RGBA

    auto* target = fragData.data();

    for (int y = 0; y < availableHeight; ++y)
    {
        auto const startOffset = static_cast<size_t>(
            ((*pixelOffset.line + y) * unbox<int>(_image->width()) + *pixelOffset.column) * 4);
        const auto* const source = &_image->get_data()[startOffset];
        target = copy(source, source + static_cast<ptrdiff_t>(availableWidth) * 4, target);

        // fill vertical gap on right
        for (int x = availableWidth; x < unbox<int>(_cellSize.width); ++x)
        {
            *target++ = _defaultColor.red();
            *target++ = _defaultColor.green();
            *target++ = _defaultColor.blue();
            *target++ = _defaultColor.alpha();
        }
    }

    // fill horizontal gap at the bottom
    for (auto y = availableHeight * unbox<int>(_cellSize.width); y < int(_cellSize.area()); ++y)
    {
        *target++ = _defaultColor.red();
        *target++ = _defaultColor.green();
        *target++ = _defaultColor.blue();
        *target++ = _defaultColor.alpha();
    }

    return fragData;
}

shared_ptr<image const> image_pool::create(image_format format, image_size size, image::data&& data)
{
    // TODO: This operation should be idempotent, i.e. if that image has been created already, return a
    // reference to that.
    auto const id = _nextImageId++;
    return make_shared<image>(id, format, std::move(data), size, _onImageRemove);
}

// TODO: Why on earth does this function exist if it's not relevant to ImagePool? Fix this mess.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
shared_ptr<rasterized_image> image_pool::rasterize(shared_ptr<image const> image,
                                                   image_alignment alignmentPolicy,
                                                   image_resize resizePolicy,
                                                   rgba_color defaultColor,
                                                   grid_size cellSpan,
                                                   image_size cellSize)
{
    return make_shared<rasterized_image>(
        std::move(image), alignmentPolicy, resizePolicy, defaultColor, cellSpan, cellSize);
}

void image_pool::link(string const& name, shared_ptr<image const> imageRef)
{
    _imageNameToImageCache.emplace(name, std::move(imageRef));
}

shared_ptr<image const> image_pool::findImageByName(string const& name) const noexcept
{
    if (auto const* imageRef = _imageNameToImageCache.try_get(name))
        return *imageRef;

    return {};
}

void image_pool::unlink(string const& name)
{
    _imageNameToImageCache.remove(name);
}

void image_pool::clear()
{
    _imageNameToImageCache.clear();
}

void image_pool::inspect(ostream& os) const
{
    os << "Image pool:\n";
    os << fmt::format("global image stats: {}\n", image_stats::get());
    _imageNameToImageCache.inspect(os);
}

} // namespace terminal
