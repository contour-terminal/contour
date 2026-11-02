// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/graphics/KittyGraphics.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/state/GridState.hpp>
#include <vtbackend/state/ImageRefs.hpp>
#include <vtbackend/state/StateCodec.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vtbackend
{

/// One Image of the table: its pixels, filed once however many things refer to it.
struct ImageState
{
    ImageId id {};                          ///< The id the image keeps across the restore.
    ImageFormat format = ImageFormat::Auto; ///< The pixel format.
    std::vector<uint8_t> data;              ///< The pixels.
    ImageSize size {};                      ///< The size in pixels.

    bool operator==(ImageState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.format);
        visitor(3, self.data);
        visitor(4, self.size);
    }
};

/// One RasterizedImage of the table, named by its index in the table.
struct RasterizedImageState
{
    ImageId image {};                                    ///< The image it rasterizes.
    ImageAlignment alignment = ImageAlignment::TopStart; ///< @see RasterizedImage::alignmentPolicy
    ImageResize resize = ImageResize::NoResize;          ///< @see RasterizedImage::resizePolicy
    RGBAColor defaultColor {};                           ///< @see RasterizedImage::defaultColor
    GridSize cellSpan {};                                ///< @see RasterizedImage::cellSpan
    ImageSize cellSize {};                               ///< @see RasterizedImage::cellSize
    ImageLayer layer = ImageLayer::Replace;              ///< @see RasterizedImage::layer
    PixelCoordinate offset {};                           ///< @see RasterizedImage::imageOffset
    ImageSize subSize {};                                ///< @see RasterizedImage::imageSubSize

    [[nodiscard]] bool operator==(RasterizedImageState const& other) const noexcept
    {
        return image == other.image && alignment == other.alignment && resize == other.resize
               && defaultColor.value == other.defaultColor.value && cellSpan.lines == other.cellSpan.lines
               && cellSpan.columns == other.cellSpan.columns && cellSize == other.cellSize
               && layer == other.layer && offset.x.value == other.offset.x.value
               && offset.y.value == other.offset.y.value && subSize == other.subSize;
    }

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.image);
        visitor(2, self.alignment);
        visitor(3, self.resize);
        visitor(4, self.defaultColor);
        visitor(5, self.cellSpan.lines);
        visitor(6, self.cellSpan.columns);
        visitor(7, self.cellSize);
        visitor(8, self.layer);
        visitor(9, self.offset.x.value);
        visitor(10, self.offset.y.value);
        visitor(11, self.subSize);
    }
};

/// One ImageFragment of the table, named by its index in the table: every cell that showed the one object
/// refers to it, and gets the one object back.
struct ImageFragmentState
{
    uint64_t rasterized = 0; ///< The index of the RasterizedImage it shows a cell of.
    CellLocation offset {};  ///< Which cell of the placement it shows.

    bool operator==(ImageFragmentState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.rasterized);
        visitor(2, self.offset);
    }
};

/// An image linked under a name in the pool.
struct NamedImageState
{
    std::string name; ///< The name.
    ImageId image {}; ///< The image it names.

    bool operator==(NamedImageState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.name);
        visitor(2, self.image);
    }
};

/// Every image, rasterized image and image fragment any part of a terminal refers to, each filed once.
///
/// The cells, the kitty tables and animation frames refer into it by index or id, which is what lets a
/// restore hand every referrer the same object again.
struct ImageTableState
{
    std::vector<ImageState> images;               ///< Each Image once, ascending by id.
    std::vector<RasterizedImageState> rasterized; ///< Each RasterizedImage once, in the order met.
    std::vector<ImageFragmentState> fragments;    ///< Each ImageFragment once, in the order met.
    std::vector<NamedImageState> names;           ///< The pool's name links, least recently used first.
    ImageId nextImageId {};                       ///< Where the pool goes on numbering.

    bool operator==(ImageTableState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.images);
        visitor(2, self.rasterized);
        visitor(3, self.fragments);
        visitor(4, self.names);
        visitor(5, self.nextImageId);
    }
};

/// One frame of a kitty animation.
struct KittyFrameState
{
    ImageId image {};            ///< The frame's pixels.
    int32_t gapMilliseconds = 0; ///< How long the frame is shown.

    bool operator==(KittyFrameState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.image);
        visitor(2, self.gapMilliseconds);
    }
};

/// The animation of one kitty image.
struct KittyAnimationState
{
    uint32_t id = 0;                     ///< The kitty image id.
    std::vector<KittyFrameState> frames; ///< Frame N of the wire is index N-1.
    uint32_t currentFrame = 1;           ///< Which frame a placement displays.
    kitty_graphics::AnimationState state = kitty_graphics::AnimationState::Unset; ///< Playback state.
    uint32_t loopCount = 0; ///< 0 unspecified, 1 forever, n>1 loop n-1 times.

    bool operator==(KittyAnimationState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.frames);
        visitor(3, self.currentFrame);
        visitor(4, self.state);
        visitor(5, self.loopCount);
    }
};

/// A chunked kitty transmission (`m=1`) in flight: the payload so far and the command that opened it.
struct KittyChunkState
{
    std::string payload;             ///< The payload accumulated across the chunks received.
    kitty_graphics::Command command; ///< The opening command; later chunks carry no control data.

    bool operator==(KittyChunkState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.payload);
        visitor(2, self.command);
    }
};

/// The kitty graphics tables of one screen.
struct KittyScreenState
{
    std::vector<std::pair<uint32_t, ImageId>> images; ///< Transmitted images by kitty id, ascending.
    std::vector<KittyAnimationState> animations;      ///< Animations, ascending by id.
    std::optional<KittyChunkState> chunked;           ///< The transmission in flight, if any.

    bool operator==(KittyScreenState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.images);
        visitor(2, self.animations);
        visitor(3, self.chunked);
    }
};

/// Collects the images a capture meets and files each once.
///
/// A capture hands it every image fragment a cell shows (as the ImageFragmentNamer the grids are captured
/// with), every image of the kitty tables and the pool's names; the table lists each object once, in the
/// order met, so the same terminal always yields the same table.
class ImageTableBuilder final: public ImageFragmentNamer
{
  public:
    /// @param pool The pool whose numbering and name links the table carries.
    explicit ImageTableBuilder(ImagePool const& pool);

    /// Registers @p image and files it.
    /// @param image The image; null is not an image and is refused by the caller's data model.
    /// @return Its id.
    ImageId addImage(std::shared_ptr<Image const> const& image);

    [[nodiscard]] uint64_t fragmentId(std::shared_ptr<ImageFragment> const& fragment) override;

    /// Finishes the table.
    /// @return The table: images ascending by id.
    [[nodiscard]] ImageTableState finish() &&;

  private:
    /// @return The index of @p raster, filed now unless it was before.
    uint64_t rasterizedIndexOf(RasterizedImage const& raster);

    ImageTableState _table;
    std::unordered_map<Image const*, ImageId> _filedImages;
    std::unordered_map<RasterizedImage const*, uint64_t> _filedRasterized;
    std::unordered_map<ImageFragment const*, uint64_t> _filedFragments;
};

/// The objects a restored image table consists of, answering the references of a restored state.
///
/// Built aside from the pool: the images are adopted under their ids, but neither the pool's numbering nor
/// its names change until commit(), so a restore that is refused after building it leaves the pool as it was
/// -- the images it built drop out of the pool's index again with their last reference.
class RestoredImageTable final: public ImageFragmentResolver
{
  public:
    /// Checks @p table and builds its objects.
    /// @param pool  The pool to adopt the images into.
    /// @param table The captured table; must outlive the result.
    /// @return The table, or InvalidValue when it is not consistent (a duplicate id, a pixmap that does not
    ///         match its size, a reference to an image, a rasterized image or a fragment it lacks).
    [[nodiscard]] static std::expected<RestoredImageTable, StateDecodeError> build(
        ImagePool& pool, ImageTableState const& table);

    [[nodiscard]] std::shared_ptr<ImageFragment> fragment(uint64_t id) const override;
    [[nodiscard]] std::shared_ptr<Image const> image(ImageId id) const override;

    /// Continues the pool's numbering after the table's and relinks its names. Cannot fail.
    /// @param pool The pool build() adopted the images into.
    void commit(ImagePool& pool) const;

  private:
    RestoredImageTable(ImageTableState const& table,
                       std::unordered_map<uint32_t, std::shared_ptr<Image const>> images,
                       std::vector<std::shared_ptr<ImageFragment>> fragments):
        _table { &table }, _images { std::move(images) }, _fragments { std::move(fragments) }
    {
    }

    ImageTableState const* _table;
    std::unordered_map<uint32_t, std::shared_ptr<Image const>> _images;
    std::vector<std::shared_ptr<ImageFragment>> _fragments;
};

/// Field list of a kitty command, for the state codec. @see FieldEncoder
namespace detail
{
    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, kitty_graphics::Command>
    void visitStateFields(Visitor& visitor, T& command)
    {
        visitor(1, command.action);
        visitor(2, command.format);
        visitor(3, command.medium);
        visitor(4, command.compression);
        visitor(5, command.imageId);
        visitor(6, command.imageNumber);
        visitor(7, command.placementId);
        visitor(8, command.pixelWidth);
        visitor(9, command.pixelHeight);
        visitor(10, command.sourceX);
        visitor(11, command.sourceY);
        visitor(12, command.sourceWidth);
        visitor(13, command.sourceHeight);
        visitor(14, command.columns);
        visitor(15, command.rows);
        visitor(16, command.zIndex);
        visitor(17, command.doNotMoveCursor);
        visitor(18, command.moreChunksFollow);
        visitor(19, command.quietOnSuccess);
        visitor(20, command.quietAlways);
        visitor(21, command.deleteTarget);
        visitor(22, command.baseFrame);
        visitor(23, command.currentFrame);
        visitor(24, command.targetFrame);
        visitor(25, command.frameGapMilliseconds);
        visitor(26, command.animationState);
        visitor(27, command.loopCount);
        visitor(28, command.frameBackground);
        visitor(29, command.compositionMode);
        visitor(30, command.payload);
    }
} // namespace detail

} // namespace vtbackend
