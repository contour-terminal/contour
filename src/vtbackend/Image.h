// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/primitives.h>

#include <crispy/StrongHash.h>
#include <crispy/StrongLRUCache.h>

#include <fmt/format.h>

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <vector>

namespace terminal
{

// XXX DRAFT
// Do we want to keep an Image that keeps the whole image together, and then cut it into grid-cell
// slices (requires reference counting on main image)?
// Or do we want to deal with Image slices right away and just keep those?
// The latter doesn't require reference counting.

enum class ImageFormat
{
    RGB,
    RGBA,
};

// clang-format off
namespace detail { struct ImageId {}; }
using ImageId = boxed::boxed<uint32_t, detail::ImageId>; // unique numerical image identifier
// clang-format on

struct ImageStats
{
    uint32_t instances = 0;
    uint32_t rasterized = 0;
    uint32_t fragments = 0;

    static ImageStats& get();
};

/**
 * Represents an image that can be displayed in the terminal by being placed into the grid cells
 */
class Image: public std::enable_shared_from_this<Image>
{
  public:
    using Data = std::vector<uint8_t>; // raw RGBA data
    using OnImageRemove = std::function<void(Image const*)>;
    /// Constructs an RGBA image.
    ///
    /// @param data      RGBA buffer data
    /// @param pixelSize image dimensionss in pixels
    Image(ImageId id, ImageFormat format, Data data, ImageSize pixelSize, OnImageRemove remover) noexcept:
        _id { id },
        _format { format },
        _data { std::move(data) },
        _size { pixelSize },
        _onImageRemove { std::move(remover) }
    {
        ++ImageStats::get().instances;
    }

    ~Image();

    Image(Image const&) = delete;
    Image& operator=(Image const&) = delete;
    Image(Image&&) noexcept = default;
    Image& operator=(Image&&) noexcept = default;

    constexpr ImageId id() const noexcept { return _id; }
    constexpr ImageFormat format() const noexcept { return _format; }
    Data const& data() const noexcept { return _data; }
    constexpr ImageSize size() const noexcept { return _size; }
    constexpr Width width() const noexcept { return _size.width; }
    constexpr Height height() const noexcept { return _size.height; }

  private:
    ImageId _id;
    ImageFormat _format;
    Data _data;
    ImageSize _size;
    OnImageRemove _onImageRemove;
};

/// Image resize hints are used to properly fit/fill the area to place the image onto.
enum class ImageResize
{
    NoResize,
    ResizeToFit, // default
    ResizeToFill,
    StretchToFill,
};

/// Image alignment policy are used to properly align the image to a given spot when not fully
/// filling the area this image as to be placed to.
enum class ImageAlignment
{
    TopStart,
    TopCenter,
    TopEnd,
    MiddleStart,
    MiddleCenter, // default
    MiddleEnd,
    BottomStart,
    BottomCenter,
    BottomEnd
};

/**
 * RasterizedImage wraps an Image into a fixed-size grid with some additional graphical properties for
 * rasterization.
 */
class RasterizedImage: public std::enable_shared_from_this<RasterizedImage>
{
  public:
    RasterizedImage(std::shared_ptr<Image const> image,
                    ImageAlignment alignmentPolicy,
                    ImageResize resizePolicy,
                    RGBAColor defaultColor,
                    GridSize cellSpan,
                    ImageSize cellSize):
        _image { std::move(image) },
        _alignmentPolicy { alignmentPolicy },
        _resizePolicy { resizePolicy },
        _defaultColor { defaultColor },
        _cellSpan { cellSpan },
        _cellSize { cellSize }
    {
        ++ImageStats::get().rasterized;
    }

    ~RasterizedImage();

    RasterizedImage(RasterizedImage const&) = delete;
    RasterizedImage(RasterizedImage&&) = delete;
    RasterizedImage& operator=(RasterizedImage const&) = delete;
    RasterizedImage& operator=(RasterizedImage&&) = delete;

    bool valid() const noexcept { return !!_image; }

    std::shared_ptr<Image const> const& imagePointer() const noexcept { return _image; }
    Image const& image() const noexcept { return *_image; }
    ImageAlignment alignmentPolicy() const noexcept { return _alignmentPolicy; }
    ImageResize resizePolicy() const noexcept { return _resizePolicy; }
    RGBAColor defaultColor() const noexcept { return _defaultColor; }
    GridSize cellSpan() const noexcept { return _cellSpan; }
    ImageSize cellSize() const noexcept { return _cellSize; }

    /// @returns an RGBA buffer for a grid cell at given coordinate @p pos of the rasterized image.
    Image::Data fragment(CellLocation pos) const;

  private:
    std::shared_ptr<Image const> _image; //!< Reference to the Image to be rasterized.
    ImageAlignment _alignmentPolicy;     //!< Alignment policy of the image inside the raster size.
    ImageResize _resizePolicy;           //!< Image resize policy
    RGBAColor _defaultColor;             //!< Default color to be applied at corners when needed.
    GridSize _cellSpan;                  //!< Number of grid cells to span the pixel image onto.
    ImageSize _cellSize;                 //!< number of pixels in X and Y dimension one grid cell has to fill.
};

/// An ImageFragment holds a graphical image that ocupies one full grid cell.
class ImageFragment
{
  public:
    ImageFragment() = delete;

    /// @param image  the Image this fragment is being cut off from
    /// @param offset 0-based grid-offset into the rasterized image
    ImageFragment(std::shared_ptr<RasterizedImage const> image, CellLocation offset):
        _rasterizedImage { std::move(image) }, _offset { offset }
    {
        ++ImageStats::get().fragments;
    }

    ImageFragment(ImageFragment const&) = delete;
    ImageFragment& operator=(ImageFragment const&) = delete;

    ImageFragment(ImageFragment&&) noexcept = default;
    ImageFragment& operator=(ImageFragment&&) noexcept = default;

    ~ImageFragment();

    [[nodiscard]] RasterizedImage const& rasterizedImage() const noexcept { return *_rasterizedImage; }

    /// @returns offset of this image fragment in pixels into the underlying image.
    CellLocation offset() const noexcept { return _offset; }

    /// Extracts the data from the image that is to be rendered.
    [[nodiscard]] Image::Data data() const { return _rasterizedImage->fragment(_offset); }

  private:
    std::shared_ptr<RasterizedImage const> _rasterizedImage;
    CellLocation _offset;
};

namespace detail
{
    struct ImageFragmentId;
}
using ImageFragmentId = boxed::boxed<uint16_t, detail::ImageFragmentId>;

inline bool operator==(ImageFragment const& a, ImageFragment const& b) noexcept
{
    return a.rasterizedImage().image().id() == b.rasterizedImage().image().id() && a.offset() == b.offset();
}

inline bool operator!=(ImageFragment const& a, ImageFragment const& b) noexcept
{
    return !(a == b);
}

inline bool operator<(ImageFragment const& a, ImageFragment const& b) noexcept
{
    return (a.rasterizedImage().image().id() < b.rasterizedImage().image().id())
           || (a.rasterizedImage().image().id() == b.rasterizedImage().image().id()
               && a.offset() < b.offset());
}

/// Highlevel Image Storage Pool.
///
/// Stores RGBA images in host memory, also taking care of eviction.
class ImagePool
{
  public:
    using OnImageRemove = std::function<void(Image const*)>;

    ImagePool(
        OnImageRemove onImageRemove = [](auto) {}, ImageId nextImageId = ImageId(1));

    /// Creates an RGBA image of given size in pixels.
    std::shared_ptr<Image const> create(ImageFormat format, ImageSize pixelSize, Image::Data&& data);

    /// Rasterizes an Image.
    std::shared_ptr<RasterizedImage> rasterize(std::shared_ptr<Image const> image,
                                               ImageAlignment alignmentPolicy,
                                               ImageResize resizePolicy,
                                               RGBAColor defaultColor,
                                               GridSize cellSpan,
                                               ImageSize cellSize);

    // named image access
    //
    void link(std::string const& name, std::shared_ptr<Image const> imageRef);
    [[nodiscard]] std::shared_ptr<Image const> findImageByName(std::string const& name) const noexcept;
    void unlink(std::string const& name);

    void inspect(std::ostream& os) const;

    void clear();

  private:
    void removeRasterizedImage(RasterizedImage* image); //!< Removes a rasterized image from pool.

    using NameToImageIdCache = crispy::strong_lru_cache<std::string, std::shared_ptr<Image const>>;

    // data members
    //
    ImageId _nextImageId;                      //!< ID for next image to be put into the pool
    NameToImageIdCache _imageNameToImageCache; //!< keeps mapping from name to raw image
    OnImageRemove _onImageRemove;              //!< Callback to be invoked when image gets removed from pool.
};

} // namespace terminal

// {{{ fmtlib support
template <>
struct fmt::formatter<terminal::ImageFormat>: formatter<std::string_view>
{
    auto format(terminal::ImageFormat value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::ImageFormat::RGB: name = "RGB"; break;
            case terminal::ImageFormat::RGBA: name = "RGBA"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::ImageStats>: formatter<std::string>
{
    auto format(terminal::ImageStats stats, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(
            fmt::format(
                "{} instances, {} raster, {} fragments", stats.instances, stats.rasterized, stats.fragments),
            ctx);
    }
};

template <>
struct fmt::formatter<std::shared_ptr<terminal::Image const>>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(std::shared_ptr<terminal::Image const> const& image, format_context& ctx)
        -> format_context::iterator
    {
        if (!image)
            return fmt::format_to(ctx.out(), "nullptr");
        terminal::Image const& imageRef = *image;
        return fmt::format_to(ctx.out(),
                              "Image<#{}, {}, size={}>",
                              imageRef.weak_from_this().use_count(),
                              imageRef.id(),
                              imageRef.size());
    }
};

template <>
struct fmt::formatter<terminal::ImageResize>: formatter<std::string_view>
{
    auto format(terminal::ImageResize value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::ImageResize::NoResize: name = "NoResize"; break;
            case terminal::ImageResize::ResizeToFit: name = "ResizeToFit"; break;
            case terminal::ImageResize::ResizeToFill: name = "ResizeToFill"; break;
            case terminal::ImageResize::StretchToFill: name = "StretchToFill"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::ImageAlignment>: formatter<std::string_view>
{
    auto format(terminal::ImageAlignment value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::ImageAlignment::TopStart: name = "TopStart"; break;
            case terminal::ImageAlignment::TopCenter: name = "TopCenter"; break;
            case terminal::ImageAlignment::TopEnd: name = "TopEnd"; break;
            case terminal::ImageAlignment::MiddleStart: name = "MiddleStart"; break;
            case terminal::ImageAlignment::MiddleCenter: name = "MiddleCenter"; break;
            case terminal::ImageAlignment::MiddleEnd: name = "MiddleEnd"; break;
            case terminal::ImageAlignment::BottomStart: name = "BottomStart"; break;
            case terminal::ImageAlignment::BottomCenter: name = "BottomCenter"; break;
            case terminal::ImageAlignment::BottomEnd: name = "BottomEnd"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::RasterizedImage>: formatter<std::string>
{
    auto format(terminal::RasterizedImage const& image, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("RasterizedImage<{}, {}, {}, {}, {}>",
                                                          image.weak_from_this().use_count(),
                                                          image.cellSpan(),
                                                          image.resizePolicy(),
                                                          image.alignmentPolicy(),
                                                          image.imagePointer()),
                                              ctx);
    }
};

template <>
struct fmt::formatter<terminal::ImageFragment>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(const terminal::ImageFragment& fragment, format_context& ctx)
        -> format_context::iterator
    {
        return fmt::format_to(
            ctx.out(), "ImageFragment<offset={}, {}>", fragment.offset(), fragment.rasterizedImage());
    }
};
// }}}
