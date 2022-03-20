// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.h>

#include <crispy/StrongLRUHashtable.h>
#include <crispy/point.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>

// clang-format off
#if __has_include(<simd>)
    #include <simd>
    namespace simd = std;
    #define VTBACKEND_SIMD_FOUND 1
#elif __has_include(<experimental/simd>) && !defined(__APPLE__) && !defined(__FreeBSD__)
    #include <experimental/simd>
    namespace simd = std::experimental;
    #define VTBACKEND_SIMD_FOUND 1
#endif
// clang-format on

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

constexpr ImageSize computeTargetSize(ImageResize resizePolicy,
                                      ImageSize imageSize,
                                      ImageSize gridSize) noexcept
{
    auto const imageWidth = unbox(imageSize.width);
    auto const imageHeight = unbox(imageSize.height);
    auto const gridWidth = unbox(gridSize.width);
    auto const gridHeight = unbox(gridSize.height);

    switch (resizePolicy)
    {
        case ImageResize::NoResize:
            return {
                .width = Width::cast_from(imageWidth),
                .height = Height::cast_from(imageHeight),
            };
        case ImageResize::ResizeToFit: {
            auto const scale = std::min(static_cast<double>(gridWidth) / static_cast<double>(imageWidth),
                                        static_cast<double>(gridHeight) / static_cast<double>(imageHeight));
            return {
                .width = Width::cast_from(static_cast<int>(imageWidth * scale)),
                .height = Height::cast_from(static_cast<int>(imageHeight * scale)),
            };
        }
        case ImageResize::ResizeToFill: {
            auto const scale = std::max(static_cast<double>(gridWidth) / static_cast<double>(imageWidth),
                                        static_cast<double>(gridHeight) / static_cast<double>(imageHeight));
            return {
                .width = Width::cast_from(static_cast<int>(imageWidth * scale)),
                .height = Height::cast_from(static_cast<int>(imageHeight * scale)),
            };
        }
        case ImageResize::StretchToFill: {
            return {
                .width = Width::cast_from(gridWidth),
                .height = Height::cast_from(gridHeight),
            };
        }
    }
    std::unreachable();
}

struct TopLeft
{
    int x {};
    int y {};
};

constexpr TopLeft computeTargetTopLeftOffset(ImageAlignment alignmentPolicy,
                                             ImageSize targetSize,
                                             ImageSize gridSize) noexcept
{
    auto const gridWidth = unbox<int>(gridSize.width);
    auto const gridHeight = unbox<int>(gridSize.height);
    auto const paramWidth = unbox<int>(targetSize.width);
    auto const paramHeight = unbox<int>(targetSize.height);
    switch (alignmentPolicy)
    {
        case ImageAlignment::TopStart: return { .x = 0, .y = 0 };
        case ImageAlignment::TopCenter: return { .x = (gridWidth - paramWidth) / 2, .y = 0 };
        case ImageAlignment::TopEnd: return { .x = gridWidth - paramWidth, .y = 0 };
        case ImageAlignment::MiddleStart: return { .x = 0, .y = (gridHeight - paramHeight) / 2 };
        case ImageAlignment::MiddleCenter:
            return { .x = (gridWidth - paramWidth) / 2, .y = (gridHeight - paramHeight) / 2 };
        case ImageAlignment::MiddleEnd:
            return { .x = gridWidth - paramWidth, .y = (gridHeight - paramHeight) / 2 };
        case ImageAlignment::BottomStart: return { .x = 0, .y = gridHeight - paramHeight };
        case ImageAlignment::BottomCenter:
            return { .x = (gridWidth - paramWidth) / 2, .y = gridHeight - paramHeight };
        case ImageAlignment::BottomEnd: return { .x = gridWidth - paramWidth, .y = gridHeight - paramHeight };
    }
    std::unreachable();
}

#if defined(VTBACKEND_SIMD_FOUND)
namespace
{
    struct SimdContext
    {
        int width;
        int cellX;
        int xOffset;
        int yOffset;
        int paramWidth;
        int paramHeight;
        int imageWidth;
        int imageHeight;
        std::vector<uint8_t> const& imageData;
        uint32_t defaultColor;
    };

    void fillFragmentSimd(int& x, uint8_t*& target, SimdContext const& context, int globalY, bool yInBounds)
    {
        using float_v = simd::native_simd<float>;
        using int_v = simd::rebind_simd_t<int, float_v>;
        constexpr int SimdWidth = float_v::size();

        for (; x + SimdWidth <= context.width; x += SimdWidth) // SIMD loop
        {
            auto const globalXVec = int_v([](int i) { return i; }) + (context.cellX + x);

            // Check bounds
            // X bounds depend on vector, Y bounds are scalar for this row
            auto const xInBounds =
                (globalXVec >= context.xOffset) && (globalXVec < (context.xOffset + context.paramWidth));

            if (yInBounds)
            {
                // Check if ALL X are in bounds
                if (simd::all_of(xInBounds))
                {
                    // Fully in bounds
                    auto const sourceXVec = simd::static_simd_cast<int>(
                        (simd::static_simd_cast<float>(globalXVec) - static_cast<float>(context.xOffset))
                        * static_cast<float>(context.imageWidth) / static_cast<float>(context.paramWidth));

                    auto const sourceY = static_cast<int>((globalY - context.yOffset)
                                                          * static_cast<double>(context.imageHeight)
                                                          / static_cast<double>(context.paramHeight));

                    for (int i = 0; i < SimdWidth; ++i)
                    {
                        auto const sourceIndex =
                            (static_cast<size_t>(sourceY) * static_cast<size_t>(context.imageWidth)
                             + static_cast<size_t>(sourceXVec[i]))
                            * 4;
                        if (sourceIndex + 4 <= context.imageData.size())
                            *(uint32_t*) target = *(uint32_t*) &context.imageData[sourceIndex];
                        else
                            *(uint32_t*) target = context.defaultColor;
                        target += 4;
                    }
                }
                else if (simd::none_of(xInBounds))
                {
                    // None (X out of bounds)
                    for (int i = 0; i < SimdWidth; ++i)
                    {
                        *(uint32_t*) target = context.defaultColor;
                        target += 4;
                    }
                }
                else
                {
                    // Mixed X
                    for (int i = 0; i < SimdWidth; ++i)
                    {
                        if (xInBounds[i])
                        {
                            auto const globalX = context.cellX + x + i;
                            auto const sourceX = static_cast<int>((globalX - context.xOffset)
                                                                  * static_cast<double>(context.imageWidth)
                                                                  / static_cast<double>(context.paramWidth));
                            auto const sourceY = static_cast<int>((globalY - context.yOffset)
                                                                  * static_cast<double>(context.imageHeight)
                                                                  / static_cast<double>(context.paramHeight));
                            auto const sourceIndex =
                                (static_cast<size_t>(sourceY) * static_cast<size_t>(context.imageWidth)
                                 + static_cast<size_t>(sourceX))
                                * 4;
                            if (sourceIndex + 4 <= context.imageData.size())
                                *(uint32_t*) target = *(uint32_t*) &context.imageData[sourceIndex];
                            else
                                *(uint32_t*) target = context.defaultColor;
                        }
                        else
                            *(uint32_t*) target = context.defaultColor;
                        target += 4;
                    }
                }
            }
            else
            {
                // Y out of bounds -> All out
                for (int i = 0; i < SimdWidth; ++i)
                {
                    *(uint32_t*) target = context.defaultColor;
                    target += 4;
                }
            }
        }
    }
} // namespace
#endif

Image::Data RasterizedImage::fragment(CellLocation pos, ImageSize targetCellSize) const
{
    auto const cellSize = targetCellSize.area() > 0 ? targetCellSize : _cellSize;
    auto const gridSize = ImageSize {
        .width = Width::cast_from(unbox<int>(_cellSpan.columns) * unbox<int>(cellSize.width)),
        .height = Height::cast_from(unbox<int>(_cellSpan.lines) * unbox<int>(cellSize.height)),
    };

    auto const imageWidth = unbox<int>(_image->width());
    auto const imageHeight = unbox<int>(_image->height());
    auto const targetSize = computeTargetSize(_resizePolicy, _image->size(), gridSize);
    auto const paramWidth = unbox<int>(targetSize.width);
    auto const paramHeight = unbox<int>(targetSize.height);
    auto const [xOffset, yOffset] = computeTargetTopLeftOffset(_alignmentPolicy, targetSize, gridSize);

    // The pixel offset of the top-left corner of the current cell in the global grid system
    auto const cellX = unbox<int>(pos.column) * unbox<int>(cellSize.width);
    auto const cellY = unbox<int>(pos.line) * unbox<int>(cellSize.height);

    auto fragmentData = Image::Data {};
    fragmentData.resize(cellSize.area() * 4); // RGBA
    uint8_t* target = fragmentData.data();

#if defined(VTBACKEND_SIMD_FOUND)
    auto const simdContext = SimdContext {
        .width = unbox<int>(cellSize.width),
        .cellX = cellX,
        .xOffset = xOffset,
        .yOffset = yOffset,
        .paramWidth = paramWidth,
        .paramHeight = paramHeight,
        .imageWidth = imageWidth,
        .imageHeight = imageHeight,
        .imageData = _image->data(),
        .defaultColor = _defaultColor.value,
    };
#endif

    // Iterate over every pixel in the CELL
    for (int const y: std::views::iota(0, unbox<int>(cellSize.height)))
    {
        auto const globalY = cellY + y;
        bool const yInBounds = (globalY >= yOffset) && (globalY < (yOffset + paramHeight));
        int x = 0;

#if defined(VTBACKEND_SIMD_FOUND)
        fillFragmentSimd(x, target, simdContext, globalY, yInBounds);
#endif

        // Scalar epilogue
        for (; x < unbox<int>(cellSize.width); ++x)
        {
            auto const globalX = cellX + x;
            if (globalX >= xOffset && globalX < xOffset + paramWidth && yInBounds)
            {
                auto const sourceX = static_cast<int>((globalX - xOffset) * static_cast<double>(imageWidth)
                                                      / static_cast<double>(paramWidth));
                auto const sourceY = static_cast<int>((globalY - yOffset) * static_cast<double>(imageHeight)
                                                      / static_cast<double>(paramHeight));
                auto const sourceIndex = (static_cast<size_t>(sourceY) * static_cast<size_t>(imageWidth)
                                          + static_cast<size_t>(sourceX))
                                         * 4;
                if (sourceIndex + 4 <= _image->data().size())
                    *(uint32_t*) target = *(uint32_t*) &_image->data()[sourceIndex];
                else
                    *(uint32_t*) target = _defaultColor.value;
            }
            else
                *(uint32_t*) target = _defaultColor.value;
            target += 4;
        }
    }

    return fragmentData;
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

void ImagePool::link(string name, shared_ptr<Image const> imageRef)
{
    _imageNameToImageCache.emplace(std::move(name), std::move(imageRef));
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
