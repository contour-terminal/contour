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
#include <terminal/Image.h>

#include <algorithm>
#include <memory>

using std::copy;
using std::min;
using std::move;
using std::shared_ptr;

namespace terminal {

Image::Data RasterizedImage::fragment(Coordinate _pos) const
{
    // TODO: respect alignment hint
    // TODO: respect resize hint

    auto const xOffset = _pos.column * unbox<int>(cellSize_.width);
    auto const yOffset = _pos.line * unbox<int>(cellSize_.height);
    auto const pixelOffset = Coordinate{yOffset, xOffset};

    Image::Data fragData;
    fragData.resize(*cellSize_.width * *cellSize_.height * 4); // RGBA
    auto const availableWidth = min(unbox<int>(image_->width()) - *pixelOffset.column, unbox<int>(cellSize_.width));
    auto const availableHeight = min(unbox<int>(image_->height()) - *pixelOffset.line, unbox<int>(cellSize_.height));

    // auto const availableSize = Size{availableWidth, availableHeight};
    // std::cout << fmt::format(
    //     "RasterizedImage.fragment({}): pixelOffset={}, cellSize={}/{}\n",
    //     _pos,
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

    // fill horizontal gap at the bottom
    for (int y = availableHeight * unbox<int>(cellSize_.width); y < *cellSize_.height * *cellSize_.width; ++y)
    {
        *target++ = defaultColor_.red();
        *target++ = defaultColor_.green();
        *target++ = defaultColor_.blue();
        *target++ = defaultColor_.alpha();
    }

    for (int y = 0; y < availableHeight; ++y)
    {
        auto const startOffset = ((*pixelOffset.line + (availableHeight - 1 - y)) * *image_->width() + *pixelOffset.column) * 4;
        auto const source = &image_->data()[startOffset];
        target = copy(source, source + availableWidth * 4, target);

        // fill vertical gap on right
        for (int x = availableWidth; x < *cellSize_.width; ++x)
        {
            *target++ = defaultColor_.red();
            *target++ = defaultColor_.green();
            *target++ = defaultColor_.blue();
            *target++ = defaultColor_.alpha();
        }
    }

    return fragData;
}

Image const& ImagePool::create(ImageFormat _format, ImageSize _size, Image::Data&& _data)
{
    // TODO: This operation should be idempotent, i.e. if that image has been created already, return a reference to that.
    auto const id = nextImageId_++;
    return images_.emplace(id, Image{id, _format, move(_data), _size});
}

std::shared_ptr<RasterizedImage const> ImagePool::rasterize(ImageId _imageId,
                                                            ImageAlignment _alignmentPolicy,
                                                            ImageResize _resizePolicy,
                                                            RGBAColor _defaultColor,
                                                            GridSize _cellSpan,
                                                            ImageSize _cellSize)
{
    rasterizedImages_.emplace_back(&images_.at(_imageId),
                                   _alignmentPolicy, _resizePolicy,
                                   _defaultColor, _cellSpan, _cellSize);
    return shared_ptr<RasterizedImage>(&rasterizedImages_.back(),
                                       [this](RasterizedImage* _image) { removeRasterizedImage(_image); });
}

void ImagePool::removeImage(Image* _image)
{
    if (auto i = find_if(images_.begin(),
                         images_.end(),
                         [&](auto const& p) { return &p.second == _image; }); i != images_.end())
    {
        onImageRemove_(_image);
        images_.erase(i);
    }
}

void ImagePool::removeRasterizedImage(RasterizedImage* _image)
{
    if (auto i = find_if(rasterizedImages_.begin(),
                         rasterizedImages_.end(),
                         [&](RasterizedImage const& p) { return &p == _image; }); i != rasterizedImages_.end())
        rasterizedImages_.erase(i);
}

void ImagePool::link(std::string const& _name, Image const& _imageRef)
{
    namedImages_[_name] = _imageRef.id();
}

Image const* ImagePool::findImageByName(std::string const& _name) const noexcept
{
    if (ImageId const* id = namedImages_.try_get(_name))
        return &images_.at(*id);

    return {};
}

void ImagePool::unlink(std::string const& _name)
{
    namedImages_.erase(_name);
}

} // end namespace
