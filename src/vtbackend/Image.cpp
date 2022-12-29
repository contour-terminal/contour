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

ImageStats& ImageStats::get()
{
    static ImageStats stats {};
    return stats;
}

Image::~Image()
{
    --ImageStats::get().instances;
    onImageRemove_(this);
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
    nextImageId_ { nextImageId },
    imageNameToImageCache_ { StrongHashtableSize { 1024 },
                             LRUCapacity { 100 },
                             "ImagePool name-to-image mappings" },
    onImageRemove_ { std::move(onImageRemove) }
{
}

Image::Data RasterizedImage::fragment(CellLocation pos) const
{
    // TODO: respect alignment hint
    // TODO: respect resize hint

    auto const xOffset = pos.column * unbox<int>(cellSize_.width);
    auto const yOffset = pos.line * unbox<int>(cellSize_.height);
    auto const pixelOffset = CellLocation { yOffset, xOffset };

    Image::Data fragData;
    fragData.resize(cellSize_.area() * 4); // RGBA
    auto const availableWidth =
        min(unbox<int>(image_->width()) - *pixelOffset.column, unbox<int>(cellSize_.width));
    auto const availableHeight =
        min(unbox<int>(image_->height()) - *pixelOffset.line, unbox<int>(cellSize_.height));

    // auto const availableSize = Size{availableWidth, availableHeight};
    // std::cout << fmt::format(
    //     "RasterizedImage.fragment({}): pixelOffset={}, cellSize={}/{}\n",
    //     pos,
    //     pixelOffset,
    //     cellSize_,
    //     availableSize
    // );

    // auto const fitsWidth = pixelOffset.column + cellSize_.width < image_.get().width();
    // auto const fitsHeight = pixelOffset.line + cellSize_.height < image_.get().height();
    // if (!fitsWidth || !fitsHeight)
    //     std::cout << fmt::format("ImageFragment: out of bounds{}{} ({}x{}); {}\n",
    //             fitsWidth ? "" : " (width)",
    //             fitsHeight ? "" : " (height)",
    //             availableWidth,
    //             availableHeight,
    //             *this);

    // TODO: if input format is (RGB | PNG), transform to RGBA

    auto target = &fragData[0];

    for (int y = 0; y < availableHeight; ++y)
    {
        auto const startOffset = static_cast<size_t>(
            ((*pixelOffset.line + y) * unbox<int>(image_->width()) + *pixelOffset.column) * 4);
        auto const source = &image_->data()[startOffset];
        target = copy(source, source + static_cast<ptrdiff_t>(availableWidth) * 4, target);

        // fill vertical gap on right
        for (int x = availableWidth; x < unbox<int>(cellSize_.width); ++x)
        {
            *target++ = defaultColor_.red();
            *target++ = defaultColor_.green();
            *target++ = defaultColor_.blue();
            *target++ = defaultColor_.alpha();
        }
    }

    // fill horizontal gap at the bottom
    for (auto y = availableHeight * unbox<int>(cellSize_.width); y < int(cellSize_.area()); ++y)
    {
        *target++ = defaultColor_.red();
        *target++ = defaultColor_.green();
        *target++ = defaultColor_.blue();
        *target++ = defaultColor_.alpha();
    }

    return fragData;
}

shared_ptr<Image const> ImagePool::create(ImageFormat format, ImageSize size, Image::Data&& data)
{
    // TODO: This operation should be idempotent, i.e. if that image has been created already, return a
    // reference to that.
    auto const id = nextImageId_++;
    return make_shared<Image>(id, format, std::move(data), size, onImageRemove_);
}

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
    imageNameToImageCache_.emplace(name, std::move(imageRef));
}

shared_ptr<Image const> ImagePool::findImageByName(string const& name) const noexcept
{
    if (auto imageRef = imageNameToImageCache_.try_get(name))
        return *imageRef;

    return {};
}

void ImagePool::unlink(string const& name)
{
    imageNameToImageCache_.remove(name);
}

void ImagePool::clear()
{
    imageNameToImageCache_.clear();
}

void ImagePool::inspect(ostream& os) const
{
    os << "Image pool:\n";
    os << fmt::format("global image stats: {}\n", ImageStats::get());
    imageNameToImageCache_.inspect(os);
}

} // namespace terminal
