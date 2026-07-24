// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/primitives.h>

#include <crispy/StrongHash.h>
#include <crispy/StrongLRUCache.h>

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vtbackend
{

// XXX DRAFT
// Do we want to keep an Image that keeps the whole image together, and then cut it into grid-cell
// slices (requires reference counting on main image)?
// Or do we want to deal with Image slices right away and just keep those?
// The latter doesn't require reference counting.

enum class ImageFormat : uint8_t
{
    Auto,
    RGB,
    RGBA,
    PNG,
};

/// Number of bytes each pixel occupies in a raw pixmap of the given format.
///
/// @param format Image format; must be a raw format (RGB or RGBA). Self-describing or unresolved
///               formats (PNG, Auto) carry no fixed stride and yield 0.
/// @return Bytes per pixel, or 0 if @p format is not a raw pixmap format.
[[nodiscard]] constexpr uint32_t bytesPerPixel(ImageFormat format) noexcept
{
    switch (format)
    {
        case ImageFormat::RGB: return 3;
        case ImageFormat::RGBA: return 4;
        case ImageFormat::Auto:
        case ImageFormat::PNG: break;
    }
    return 0;
}

/// Tests whether a raw pixmap is exactly as long as its declared geometry claims.
///
/// Every path that turns wire data into an Image must pass this before uploading: the renderer
/// uploads the declared geometry to the GPU and reads `height * width * 4` bytes from the buffer,
/// so a pixmap shorter than its geometry is read out of bounds.
///
/// @param format    Image format. PNG is self-describing and always passes; Auto must be resolved first.
/// @param size      Declared image dimensions in pixels.
/// @param dataSize  Length of the pixmap buffer, in bytes.
/// @return true if the buffer matches @p size exactly (or @p format is self-describing).
[[nodiscard]] constexpr bool isConsistentPixmap(ImageFormat format, ImageSize size, size_t dataSize) noexcept
{
    if (format == ImageFormat::PNG)
        return true; // self-describing: the decoder validates it
    if (format == ImageFormat::Auto)
        return false; // must be resolved to a concrete format before it can be checked
    if (*size.width <= 0 || *size.height <= 0)
        return false;
    // Checked multiply: width * height * bpp can overflow size_t for extreme
    // (e.g. 65537² RGBA) images, wrapping to a value that spuriously matches
    // dataSize and letting an undersized pixmap through validation.
    auto const w = static_cast<size_t>(*size.width);
    auto const h = static_cast<size_t>(*size.height);
    auto const bpp = static_cast<size_t>(bytesPerPixel(format));
    auto const pixels = w * h;
    if (w != 0 && pixels / w != h) // overflow in w * h
        return false;
    auto const expected = pixels * bpp;
    if (pixels != 0 && expected / pixels != bpp) // overflow in pixels * bpp
        return false;
    return dataSize == expected;
}

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

/// Image layer determines the z-ordering of the image relative to text.
///
/// @see GoodImageProtocol spec, parameter `L`.
enum class ImageLayer : uint8_t
{
    Below = 0,   ///< Render below text (watermark-like).
    Replace = 1, ///< Replace text cells (default, like Sixel).
    Above = 2,   ///< Render above text (overlay).
};

/// Image resize hints are used to properly fit/fill the area to place the image onto.
enum class ImageResize : uint8_t
{
    NoResize,
    ResizeToFit, // default
    ResizeToFill,
    StretchToFill,
};

/// Image alignment policy are used to properly align the image to a given spot when not fully
/// filling the area this image as to be placed to.
enum class ImageAlignment : uint8_t
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

/// Where one grid cell's image content lies, expressed as geometry.
///
/// The alignment and resize policies can letterbox an image inside its cell span, so a cell may be
/// covered entirely, partially, or not at all. Everything here is in the coordinate system a
/// renderer needs: target in cell-local pixels, source normalized against the image.
struct FragmentPlacement
{
    /// False when the cell holds no image pixels at all, i.e. it lies wholly in the gap.
    bool hasImage = false;

    /// Cell-local pixel offset of the covered area.
    int targetX = 0;
    int targetY = 0;

    /// Size of the covered area, in pixels.
    ImageSize targetSize {};

    /// Normalized (0..1) source rectangle into the image backing the covered area.
    float sourceX = 0.0f;
    float sourceY = 0.0f;
    float sourceWidth = 0.0f;
    float sourceHeight = 0.0f;

    /// True when the covered area is the whole cell, i.e. there is no gap left to fill.
    bool coversCell = false;
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
                    ImageSize cellSize,
                    ImageLayer layer = ImageLayer::Replace,
                    PixelCoordinate imageOffset = {},
                    ImageSize imageSubSize = {}):
        _image { std::move(image) },
        _alignmentPolicy { alignmentPolicy },
        _resizePolicy { resizePolicy },
        _defaultColor { defaultColor },
        _cellSpan { cellSpan },
        _cellSize { cellSize },
        _layer { layer },
        _imageOffset { imageOffset },
        _imageSubSize { imageSubSize }
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
    ImageLayer layer() const noexcept { return _layer; }

    /// @returns an RGBA buffer for a grid cell at given coordinate @p pos of the rasterized image.
    ///
    /// @param pos            0-based cell location inside the rasterized image.
    /// @param targetCellSize Optional target cell size in pixels; if not given, the cell size
    ///                       as given at construction time is used.
    Image::Data fragment(CellLocation pos, ImageSize targetCellSize = {}) const;

    /// @returns where a grid cell's image content lies, as geometry rather than as pixels.
    ///
    /// fragment() answers the same question one pixel at a time and materializes an RGBA buffer for
    /// it. This answers it once per cell, as a source rectangle and a target rectangle, so a
    /// renderer can sample the image directly instead of resampling it on the CPU.
    ///
    /// @param pos            0-based cell location inside the rasterized image.
    /// @param targetCellSize Optional target cell size in pixels; if not given, the cell size
    ///                       as given at construction time is used.
    [[nodiscard]] FragmentPlacement fragmentPlacement(CellLocation pos, ImageSize targetCellSize = {}) const;

  private:
    std::shared_ptr<Image const> _image; //!< Reference to the Image to be rasterized.
    ImageAlignment _alignmentPolicy;     //!< Alignment policy of the image inside the raster size.
    ImageResize _resizePolicy;           //!< Image resize policy
    RGBAColor _defaultColor;             //!< Default color to be applied at corners when needed.
    GridSize _cellSpan;                  //!< Number of grid cells to span the pixel image onto.
    ImageSize _cellSize;                 //!< number of pixels in X and Y dimension one grid cell has to fill.
    ImageLayer _layer;                   //!< Layer for z-ordering relative to text.
    PixelCoordinate _imageOffset;        //!< Pixel offset into the source image for sub-rectangle rendering.
    ImageSize _imageSubSize;             //!< Sub-region pixel size (zero means full image).
};

std::shared_ptr<RasterizedImage> rasterize(std::shared_ptr<Image const> image,
                                           ImageAlignment alignmentPolicy,
                                           ImageResize resizePolicy,
                                           RGBAColor defaultColor,
                                           GridSize cellSpan,
                                           ImageSize cellSize,
                                           ImageLayer layer = ImageLayer::Replace);

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

/// Returns true if the image fragment should be preserved when text is written to a cell.
///
/// Below and Above layer images coexist with text; only Replace layer images are destroyed.
[[nodiscard]] inline bool shouldPreserveImageOnTextWrite(
    std::shared_ptr<ImageFragment> const& fragment) noexcept
{
    if (!fragment)
        return false;
    return fragment->rasterizedImage().layer() != ImageLayer::Replace;
}

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

    ImagePool(OnImageRemove onImageRemove = [](auto) {}, ImageId nextImageId = ImageId(1));

    /// Creates an RGBA image of given size in pixels.
    std::shared_ptr<Image const> create(ImageFormat format, ImageSize pixelSize, Image::Data&& data);

    // named image access
    //
    void link(std::string const& name, std::shared_ptr<Image const> imageRef);
    [[nodiscard]] std::shared_ptr<Image const> findImageByName(std::string const& name) const noexcept;
    void unlink(std::string const& name);

    /// Finds a live image by its stable id.
    /// @param id The image id as referenced by grid cells.
    /// @return The image, or nullptr when it was never created or its last reference is
    ///         gone (the daemon answers a FetchImage request with ImageGone then).
    /// @pre The caller must ensure the ImagePool outlives this call; the pool reference
    ///      is not internally guarded — a dangling pool pointer is use-after-free.
    [[nodiscard]] std::shared_ptr<Image const> findImageById(ImageId id) const;

    void inspect(std::ostream& os) const;

    void clear();

  private:
    /// Test-only seam: rewinds _nextImageId to reproduce the uint32 id-counter wrap
    /// (a reused id colliding with a still-live image) without four billion allocations.
    friend struct ImagePoolIdWrapTester;

    void removeRasterizedImage(RasterizedImage* image); //!< Removes a rasterized image from pool.

    using NameToImageIdCache = crispy::strong_lru_cache<std::string, std::shared_ptr<Image const>>;

    /// The id index behind findImageById(): weak_ptrs so the index never extends image
    /// lifetime (eviction stays refcount-driven), shared with each image's remover so
    /// pruning is safe regardless of destruction order, and mutex-guarded because the
    /// last reference can drop on any thread (e.g. the GUI's render thread).
    struct IdIndex
    {
        std::mutex mutex;
        std::unordered_map<uint32_t, std::weak_ptr<Image const>> images;
    };

    // data members
    //
    ImageId _nextImageId;                      //!< ID for next image to be put into the pool
    NameToImageIdCache _imageNameToImageCache; //!< keeps mapping from name to raw image
    OnImageRemove _onImageRemove;              //!< Callback to be invoked when image gets removed from pool.
    std::shared_ptr<IdIndex> _idIndex = std::make_shared<IdIndex>(); //!< id -> live image index
};

} // namespace vtbackend

// {{{ fmtlib support
template <>
struct std::formatter<vtbackend::ImageFormat>: formatter<std::string_view>
{
    auto format(vtbackend::ImageFormat value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ImageFormat::Auto: name = "Auto"; break;
            case vtbackend::ImageFormat::RGB: name = "RGB"; break;
            case vtbackend::ImageFormat::RGBA: name = "RGBA"; break;
            case vtbackend::ImageFormat::PNG: name = "PNG"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ImageStats>: formatter<std::string>
{
    auto format(vtbackend::ImageStats stats, auto& ctx) const
    {
        return formatter<std::string>::format(
            std::format(
                "{} instances, {} raster, {} fragments", stats.instances, stats.rasterized, stats.fragments),
            ctx);
    }
};

template <>
struct std::formatter<std::shared_ptr<vtbackend::Image const>>: std::formatter<std::string>
{
    auto format(std::shared_ptr<vtbackend::Image const> const& image, auto& ctx) const
    {
        std::string text;
        if (!image)
            text = "nullptr";
        else
        {
            vtbackend::Image const& imageRef = *image;
            text = std::format("Image<#{}, {}, size={}x{}>",
                               imageRef.weak_from_this().use_count(),
                               imageRef.id(),
                               imageRef.size().width.value,
                               imageRef.size().height.value);
        }
        return formatter<std::string>::format(text, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ImageLayer>: formatter<std::string_view>
{
    auto format(vtbackend::ImageLayer value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ImageLayer::Below: name = "Below"; break;
            case vtbackend::ImageLayer::Replace: name = "Replace"; break;
            case vtbackend::ImageLayer::Above: name = "Above"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ImageResize>: formatter<std::string_view>
{
    auto format(vtbackend::ImageResize value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ImageResize::NoResize: name = "NoResize"; break;
            case vtbackend::ImageResize::ResizeToFit: name = "ResizeToFit"; break;
            case vtbackend::ImageResize::ResizeToFill: name = "ResizeToFill"; break;
            case vtbackend::ImageResize::StretchToFill: name = "StretchToFill"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ImageAlignment>: formatter<std::string_view>
{
    auto format(vtbackend::ImageAlignment value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ImageAlignment::TopStart: name = "TopStart"; break;
            case vtbackend::ImageAlignment::TopCenter: name = "TopCenter"; break;
            case vtbackend::ImageAlignment::TopEnd: name = "TopEnd"; break;
            case vtbackend::ImageAlignment::MiddleStart: name = "MiddleStart"; break;
            case vtbackend::ImageAlignment::MiddleCenter: name = "MiddleCenter"; break;
            case vtbackend::ImageAlignment::MiddleEnd: name = "MiddleEnd"; break;
            case vtbackend::ImageAlignment::BottomStart: name = "BottomStart"; break;
            case vtbackend::ImageAlignment::BottomCenter: name = "BottomCenter"; break;
            case vtbackend::ImageAlignment::BottomEnd: name = "BottomEnd"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::RasterizedImage>: formatter<std::string>
{
    auto format(vtbackend::RasterizedImage const& image, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("RasterizedImage<{}, {}, {}, {}, {}>",
                                                          image.weak_from_this().use_count(),
                                                          image.cellSpan(),
                                                          image.resizePolicy(),
                                                          image.alignmentPolicy(),
                                                          image.imagePointer()->id().value),
                                              ctx);
    }
};

template <>
struct std::formatter<vtbackend::ImageFragment>: std::formatter<std::string>
{
    auto format(vtbackend::ImageFragment const& fragment, auto& ctx) const
    {
        return formatter<std::string>::format(
            std::format("ImageFragment<offset={}, {}>",
                        fragment.offset().line.value,
                        fragment.offset().column.value,
                        fragment.rasterizedImage().imagePointer()->id().value),
            ctx);
    }
};
// }}}
