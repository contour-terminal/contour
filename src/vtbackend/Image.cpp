// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.h>

#include <crispy/StrongLRUHashtable.h>

#include <algorithm>
#include <memory>
#include <utility>

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

Image::Data RasterizedImage::fragment(CellLocation pos, ImageSize targetCellSize) const
{
    auto const cellSize = targetCellSize.area() > 0 ? targetCellSize : _cellSize;
    auto const gridWidth = unbox<int>(_cellSpan.columns) * unbox<int>(cellSize.width);
    auto const gridHeight = unbox<int>(_cellSpan.lines) * unbox<int>(cellSize.height);

    auto const imageWidth = unbox<int>(_image->width());
    auto const imageHeight = unbox<int>(_image->height());

    auto const [paramWidth,
                paramHeight] = [this, imageWidth, imageHeight, gridWidth, gridHeight]() { // Target Size
        switch (_resizePolicy)
        {
            case ImageResize::NoResize: return std::pair { imageWidth, imageHeight };
            case ImageResize::ResizeToFit: {
                auto const scale =
                    std::min(static_cast<double>(gridWidth) / static_cast<double>(imageWidth),
                             static_cast<double>(gridHeight) / static_cast<double>(imageHeight));
                return std::pair { static_cast<int>(imageWidth * scale),
                                   static_cast<int>(imageHeight * scale) };
            }
            case ImageResize::ResizeToFill: {
                auto const scale =
                    std::max(static_cast<double>(gridWidth) / static_cast<double>(imageWidth),
                             static_cast<double>(gridHeight) / static_cast<double>(imageHeight));
                return std::pair { static_cast<int>(imageWidth * scale),
                                   static_cast<int>(imageHeight * scale) };
            }
            case ImageResize::StretchToFill: return std::pair { gridWidth, gridHeight };
        }
        return std::pair { imageWidth, imageHeight };
    }();

    auto const [xOffset, yOffset] = [this, gridWidth, gridHeight, paramWidth, paramHeight]() { // TopLeft
        switch (_alignmentPolicy)
        {
            case ImageAlignment::TopStart: return std::pair { 0, 0 };
            case ImageAlignment::TopCenter: return std::pair { (gridWidth - paramWidth) / 2, 0 };
            case ImageAlignment::TopEnd: return std::pair { gridWidth - paramWidth, 0 };
            case ImageAlignment::MiddleStart: return std::pair { 0, (gridHeight - paramHeight) / 2 };
            case ImageAlignment::MiddleCenter:
                return std::pair { (gridWidth - paramWidth) / 2, (gridHeight - paramHeight) / 2 };
            case ImageAlignment::MiddleEnd:
                return std::pair { gridWidth - paramWidth, (gridHeight - paramHeight) / 2 };
            case ImageAlignment::BottomStart: return std::pair { 0, gridHeight - paramHeight };
            case ImageAlignment::BottomCenter:
                return std::pair { (gridWidth - paramWidth) / 2, gridHeight - paramHeight };
            case ImageAlignment::BottomEnd:
                return std::pair { gridWidth - paramWidth, gridHeight - paramHeight };
        }
        return std::pair { 0, 0 };
    }();

    // The pixel offset of the top-left corner of the current cell in the global grid system
    auto const cellX = unbox<int>(pos.column) * unbox<int>(cellSize.width);
    auto const cellY = unbox<int>(pos.line) * unbox<int>(cellSize.height);

    Image::Data fragData;
    fragData.resize(cellSize.area() * 4); // RGBA
    auto* target = fragData.data();

    // Iterate over every pixel in the CELL
    for (int y = 0; y < unbox<int>(cellSize.height); ++y)
    {
        for (int x = 0; x < unbox<int>(cellSize.width); ++x)
        {
            // Global coordinate of the pixel we are rendering
            auto const globalX = cellX + x;
            auto const globalY = cellY + y;

            // Check if this global pixel is within the image's target rectangle
            if (globalX >= xOffset && globalX < xOffset + paramWidth && globalY >= yOffset
                && globalY < yOffset + paramHeight)
            {
                // Map global coordinate to source image coordinate
                // globalX - xOffset is the x-coordinate relative to the image's top-left
                // Then scale it back to the source image size
                // We use integer arithmetic carefully or double? Image is unlikely to be > 2B pixels.
                // Using double for precision in scaling mapping.
                auto const sourceX = static_cast<int>((globalX - xOffset) * static_cast<double>(imageWidth)
                                                      / static_cast<double>(paramWidth));
                auto const sourceY = static_cast<int>((globalY - yOffset) * static_cast<double>(imageHeight)
                                                      / static_cast<double>(paramHeight));

                auto const sourceIndex = (static_cast<size_t>(sourceY) * static_cast<size_t>(imageWidth)
                                          + static_cast<size_t>(sourceX))
                                         * 4;
                if (sourceIndex + 4 <= _image->data().size())
                {
                    const auto* const source = &_image->data()[sourceIndex];
                    *target++ = source[0];
                    *target++ = source[1];
                    *target++ = source[2];
                    *target++ = source[3];
                }
                else
                {
                    *target++ = _defaultColor.red();
                    *target++ = _defaultColor.green();
                    *target++ = _defaultColor.blue();
                    *target++ = _defaultColor.alpha();
                }
            }
            else
            {
                *target++ = _defaultColor.red();
                *target++ = _defaultColor.green();
                *target++ = _defaultColor.blue();
                *target++ = _defaultColor.alpha();
            }
        }
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

shared_ptr<RasterizedImage> rasterize(shared_ptr<Image const> image,
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
    os << std::format("global image stats: {}\n", ImageStats::get());
    _imageNameToImageCache.inspect(os);
}

} // namespace vtbackend
