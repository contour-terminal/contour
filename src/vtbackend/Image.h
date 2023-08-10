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

enum class image_format
{
    RGB,
    RGBA,
};

// clang-format off
namespace detail { struct image_id {}; }
using image_id = crispy::boxed<uint32_t, detail::image_id>; // unique numerical image identifier
// clang-format on

struct image_stats
{
    uint32_t instances = 0;
    uint32_t rasterized = 0;
    uint32_t fragments = 0;

    static image_stats& get();
};

/**
 * Represents an image that can be displayed in the terminal by being placed into the grid cells
 */
class image: public std::enable_shared_from_this<image>
{
  public:
    using data = std::vector<uint8_t>; // raw RGBA data
    using on_image_remove = std::function<void(image const*)>;
    /// Constructs an RGBA image.
    ///
    /// @param data      RGBA buffer data
    /// @param pixelSize image dimensionss in pixels
    image(image_id id, image_format format, data data, image_size pixelSize, on_image_remove remover) noexcept
        :
        _id { id },
        _format { format },
        _data { std::move(data) },
        _size { pixelSize },
        _onImageRemove { std::move(remover) }
    {
        ++image_stats::get().instances;
    }

    ~image();

    image(image const&) = delete;
    image& operator=(image const&) = delete;
    image(image&&) noexcept = default;
    image& operator=(image&&) noexcept = default;

    constexpr image_id id() const noexcept { return _id; }
    constexpr image_format format() const noexcept { return _format; }
    data const& get_data() const noexcept { return _data; }
    constexpr image_size size() const noexcept { return _size; }
    constexpr width width() const noexcept { return _size.width; }
    constexpr height height() const noexcept { return _size.height; }

  private:
    image_id _id;
    image_format _format;
    data _data;
    image_size _size;
    on_image_remove _onImageRemove;
};

/// Image resize hints are used to properly fit/fill the area to place the image onto.
enum class image_resize
{
    NoResize,
    ResizeToFit, // default
    ResizeToFill,
    StretchToFill,
};

/// Image alignment policy are used to properly align the image to a given spot when not fully
/// filling the area this image as to be placed to.
enum class image_alignment
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
class rasterized_image: public std::enable_shared_from_this<rasterized_image>
{
  public:
    rasterized_image(std::shared_ptr<image const> image,
                     image_alignment alignmentPolicy,
                     image_resize resizePolicy,
                     rgba_color defaultColor,
                     grid_size cellSpan,
                     image_size cellSize):
        _image { std::move(image) },
        _alignmentPolicy { alignmentPolicy },
        _resizePolicy { resizePolicy },
        _defaultColor { defaultColor },
        _cellSpan { cellSpan },
        _cellSize { cellSize }
    {
        ++image_stats::get().rasterized;
    }

    ~rasterized_image();

    rasterized_image(rasterized_image const&) = delete;
    rasterized_image(rasterized_image&&) = delete;
    rasterized_image& operator=(rasterized_image const&) = delete;
    rasterized_image& operator=(rasterized_image&&) = delete;

    bool valid() const noexcept { return !!_image; }

    std::shared_ptr<image const> const& imagePointer() const noexcept { return _image; }
    image const& image() const noexcept { return *_image; }
    image_alignment alignmentPolicy() const noexcept { return _alignmentPolicy; }
    image_resize resizePolicy() const noexcept { return _resizePolicy; }
    rgba_color defaultColor() const noexcept { return _defaultColor; }
    grid_size cellSpan() const noexcept { return _cellSpan; }
    image_size cellSize() const noexcept { return _cellSize; }

    /// @returns an RGBA buffer for a grid cell at given coordinate @p pos of the rasterized image.
    image::data fragment(cell_location pos) const;

  private:
    std::shared_ptr<terminal::image const> _image; //!< Reference to the Image to be rasterized.
    image_alignment _alignmentPolicy;              //!< Alignment policy of the image inside the raster size.
    image_resize _resizePolicy;                    //!< Image resize policy
    rgba_color _defaultColor;                      //!< Default color to be applied at corners when needed.
    grid_size _cellSpan;                           //!< Number of grid cells to span the pixel image onto.
    image_size _cellSize; //!< number of pixels in X and Y dimension one grid cell has to fill.
};

/// An ImageFragment holds a graphical image that ocupies one full grid cell.
class image_fragment
{
  public:
    image_fragment() = delete;

    /// @param image  the Image this fragment is being cut off from
    /// @param offset 0-based grid-offset into the rasterized image
    image_fragment(std::shared_ptr<rasterized_image const> image, cell_location offset):
        _rasterizedImage { std::move(image) }, _offset { offset }
    {
        ++image_stats::get().fragments;
    }

    image_fragment(image_fragment const&) = delete;
    image_fragment& operator=(image_fragment const&) = delete;

    image_fragment(image_fragment&&) noexcept = default;
    image_fragment& operator=(image_fragment&&) noexcept = default;

    ~image_fragment();

    [[nodiscard]] rasterized_image const& rasterizedImage() const noexcept { return *_rasterizedImage; }

    /// @returns offset of this image fragment in pixels into the underlying image.
    cell_location offset() const noexcept { return _offset; }

    /// Extracts the data from the image that is to be rendered.
    [[nodiscard]] image::data data() const { return _rasterizedImage->fragment(_offset); }

  private:
    std::shared_ptr<rasterized_image const> _rasterizedImage;
    cell_location _offset;
};

namespace detail
{
    struct image_fragment_id;
}
using image_fragment_id = crispy::boxed<uint16_t, detail::image_fragment_id>;

inline bool operator==(image_fragment const& a, image_fragment const& b) noexcept
{
    return a.rasterizedImage().image().id() == b.rasterizedImage().image().id() && a.offset() == b.offset();
}

inline bool operator!=(image_fragment const& a, image_fragment const& b) noexcept
{
    return !(a == b);
}

inline bool operator<(image_fragment const& a, image_fragment const& b) noexcept
{
    return (a.rasterizedImage().image().id() < b.rasterizedImage().image().id())
           || (a.rasterizedImage().image().id() == b.rasterizedImage().image().id()
               && a.offset() < b.offset());
}

/// Highlevel Image Storage Pool.
///
/// Stores RGBA images in host memory, also taking care of eviction.
class image_pool
{
  public:
    using on_image_remove = std::function<void(image const*)>;

    image_pool(
        on_image_remove onImageRemove = [](auto) {}, image_id nextImageId = image_id(1));

    /// Creates an RGBA image of given size in pixels.
    std::shared_ptr<image const> create(image_format format, image_size pixelSize, image::data&& data);

    /// Rasterizes an Image.
    std::shared_ptr<rasterized_image> rasterize(std::shared_ptr<image const> image,
                                                image_alignment alignmentPolicy,
                                                image_resize resizePolicy,
                                                rgba_color defaultColor,
                                                grid_size cellSpan,
                                                image_size cellSize);

    // named image access
    //
    void link(std::string const& name, std::shared_ptr<image const> imageRef);
    [[nodiscard]] std::shared_ptr<image const> findImageByName(std::string const& name) const noexcept;
    void unlink(std::string const& name);

    void inspect(std::ostream& os) const;

    void clear();

  private:
    void removeRasterizedImage(rasterized_image* image); //!< Removes a rasterized image from pool.

    using name_to_image_id_cache = crispy::StrongLRUCache<std::string, std::shared_ptr<image const>>;

    // data members
    //
    image_id _nextImageId;                         //!< ID for next image to be put into the pool
    name_to_image_id_cache _imageNameToImageCache; //!< keeps mapping from name to raw image
    on_image_remove _onImageRemove; //!< Callback to be invoked when image gets removed from pool.
};

} // namespace terminal

// {{{ fmtlib support
template <>
struct fmt::formatter<terminal::image_format>: formatter<std::string_view>
{
    auto format(terminal::image_format value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::image_format::RGB: name = "RGB"; break;
            case terminal::image_format::RGBA: name = "RGBA"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::image_stats>: formatter<std::string>
{
    auto format(terminal::image_stats stats, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(
            fmt::format(
                "{} instances, {} raster, {} fragments", stats.instances, stats.rasterized, stats.fragments),
            ctx);
    }
};

template <>
struct fmt::formatter<std::shared_ptr<terminal::image const>>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(std::shared_ptr<terminal::image const> const& image, format_context& ctx)
        -> format_context::iterator
    {
        if (!image)
            return fmt::format_to(ctx.out(), "nullptr");
        terminal::image const& imageRef = *image;
        return fmt::format_to(ctx.out(),
                              "Image<#{}, {}, size={}>",
                              imageRef.weak_from_this().use_count(),
                              imageRef.id(),
                              imageRef.size());
    }
};

template <>
struct fmt::formatter<terminal::image_resize>: formatter<std::string_view>
{
    auto format(terminal::image_resize value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::image_resize::NoResize: name = "NoResize"; break;
            case terminal::image_resize::ResizeToFit: name = "ResizeToFit"; break;
            case terminal::image_resize::ResizeToFill: name = "ResizeToFill"; break;
            case terminal::image_resize::StretchToFill: name = "StretchToFill"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::image_alignment>: formatter<std::string_view>
{
    auto format(terminal::image_alignment value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::image_alignment::TopStart: name = "TopStart"; break;
            case terminal::image_alignment::TopCenter: name = "TopCenter"; break;
            case terminal::image_alignment::TopEnd: name = "TopEnd"; break;
            case terminal::image_alignment::MiddleStart: name = "MiddleStart"; break;
            case terminal::image_alignment::MiddleCenter: name = "MiddleCenter"; break;
            case terminal::image_alignment::MiddleEnd: name = "MiddleEnd"; break;
            case terminal::image_alignment::BottomStart: name = "BottomStart"; break;
            case terminal::image_alignment::BottomCenter: name = "BottomCenter"; break;
            case terminal::image_alignment::BottomEnd: name = "BottomEnd"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::rasterized_image>: formatter<std::string>
{
    auto format(terminal::rasterized_image const& image, format_context& ctx) -> format_context::iterator
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
struct fmt::formatter<terminal::image_fragment>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(const terminal::image_fragment& fragment, format_context& ctx)
        -> format_context::iterator
    {
        return fmt::format_to(
            ctx.out(), "ImageFragment<offset={}, {}>", fragment.offset(), fragment.rasterizedImage());
    }
};
// }}}
