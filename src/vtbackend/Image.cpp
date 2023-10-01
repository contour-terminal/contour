// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.h>

#include <algorithm>
#include <memory>

#include "crispy/StrongLRUHashtable.h"

using std::copy;
using std::make_shared;
using std::min;
using std::move;
using std::ostream;
using std::shared_ptr;
using std::string;

namespace vtbackend
{

ImageStats& ImageStats::get()
{
    static ImageStats stats {};
    return stats;
}

Image::~Image()
{
    --ImageStats::get().instances;
    _onImageRemove(this);
}

RasterizedImage::~RasterizedImage()
{
    --ImageStats::get().rasterized;
}

ImageFragment::~ImageFragment()
{
    --ImageStats::get().fragments;
}

ImagePool::ImagePool(OnImageRemove onImageRemove, ImageId nextImageId):
    _nextImageId { nextImageId },
    _imageNameToImageCache { crispy::strong_hashtable_size { 1024 },
                             crispy::lru_capacity { 100 },
                             "ImagePool name-to-image mappings" },
    _onImageRemove { std::move(onImageRemove) }
{
}

Image::Data RasterizedImage::fragment(CellLocation pos) const
{
    // TODO: respect alignment hint
    // TODO: respect resize hint

    auto const xOffset = pos.column * unbox<int>(_cellSize.width);
    auto const yOffset = pos.line * unbox<int>(_cellSize.height);
    auto const pixelOffset = CellLocation { yOffset, xOffset };

    Image::Data fragData;
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
        const auto* const source = &_image->data()[startOffset];
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

shared_ptr<Image const> ImagePool::create(ImageFormat format, ImageSize size, Image::Data&& data)
{
    // TODO: This operation should be idempotent, i.e. if that image has been created already, return a
    // reference to that.
    auto const id = _nextImageId++;
    return make_shared<Image>(id, format, std::move(data), size, _onImageRemove);
}

// TODO: Why on earth does this function exist if it's not relevant to ImagePool? Fix this mess.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
shared_ptr<RasterizedImage> ImagePool::rasterize(shared_ptr<Image const> image,
                                                 ImageAlignment alignmentPolicy,
                                                 ImageResize resizePolicy,
                                                 RGBAColor defaultColor,
                                                 GridSize cellSpan,
                                                 ImageSize cellSize)
{
    return make_shared<RasterizedImage>(
        std::move(image), alignmentPolicy, resizePolicy, defaultColor, cellSpan, cellSize);
}

void ImagePool::link(string const& name, shared_ptr<Image const> imageRef)
{
    _imageNameToImageCache.emplace(name, std::move(imageRef));
}

shared_ptr<Image const> ImagePool::findImageByName(string const& name) const noexcept
{
    if (auto const* imageRef = _imageNameToImageCache.try_get(name))
        return *imageRef;

    return {};
}

void ImagePool::unlink(string const& name)
{
    _imageNameToImageCache.remove(name);
}

void ImagePool::clear()
{
    _imageNameToImageCache.clear();
}

void ImagePool::inspect(ostream& os) const
{
    os << "Image pool:\n";
    os << fmt::format("global image stats: {}\n", ImageStats::get());
    _imageNameToImageCache.inspect(os);
}

} // namespace vtbackend
