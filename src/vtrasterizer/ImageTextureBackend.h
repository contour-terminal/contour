// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtrasterizer/TextureAtlas.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace vtrasterizer::atlas
{

/// Identifies one whole-image texture living on the backend.
///
/// Distinct from vtbackend::ImageId: an image only gets a texture while something on screen
/// references it, and the backend is free to recycle ids of textures it has destroyed.
struct ImageTextureId
{
    uint32_t value {};

    constexpr bool operator==(ImageTextureId const& other) const noexcept { return value == other.value; }
    constexpr bool operator!=(ImageTextureId const& other) const noexcept { return !(*this == other); }
};

/// Command: create a texture holding one complete image.
struct CreateImageTexture
{
    ImageTextureId id;
    vtbackend::ImageSize size;
    Format format;
    /// The pixels to upload; exactly @c size worth of them. Borrowed, not owned: valid for as long as
    /// @c owner is held, which is why the command carries the owner along with it.
    std::span<uint8_t const> data;
    /// Keeps the storage behind @c data alive until this queued command executes -- either the source
    /// image, whose pixels upload straight out of its own buffer, or a widened copy of them. Uploading
    /// a full-screen image would otherwise copy tens of megabytes on the render thread.
    std::shared_ptr<void const> owner;
};

/// Command: release the texture behind @p id.
struct DestroyImageTexture
{
    ImageTextureId id;
};

/// Command: draw a sub-rectangle of an image texture into a target rectangle.
struct RenderImageQuad
{
    ImageTextureId texture;
    int x {}; ///< target left, in item pixels
    int y {}; ///< target top, in item pixels
    vtbackend::ImageSize targetSize;
    NormalizedTileLocation source; ///< normalized sub-rect of the source texture
    std::array<float, 4> color {}; ///< tint, applied as-is by the fragment shader
    /// Whether this quad composites over the text or under it. Every vertex sits at the same depth,
    /// so draw order is the only thing that expresses z; the backend needs to know which side of the
    /// text pass to place the quad on.
    bool aboveText = true;
};

/// Command: fill a rectangle with a solid colour, ordered among the image quads.
///
/// The alignment gap around an image belongs to the image's own composite, not to the cell background
/// beneath it: an image drawn above the text used to occlude that text out to the edge of its cells,
/// because the whole cell was painted. A fill issued through the background path could not do that -- it
/// composites before the text, whichever order it was issued in.
struct RenderImageGap
{
    int x {}; ///< target left, in item pixels
    int y {}; ///< target top, in item pixels
    vtbackend::ImageSize size;
    vtbackend::RGBAColor color;
    /// Which side of the text pass this fill belongs on; see RenderImageQuad::aboveText.
    bool aboveText = true;
};

/// Draws whole images, as opposed to AtlasBackend's fixed-size tiles.
///
/// This is deliberately a sibling of AtlasBackend rather than a growth of it. An image is not a
/// tile: it has an arbitrary size, it is sampled by sub-rectangle, and it is uploaded once and then
/// only drawn. Forcing images through a fixed tile grid is what makes a large image evict its own
/// tiles mid-frame and render as garbage.
///
/// Ordering contract: commands issued through this interface and through AtlasBackend composite in
/// the order they were issued.
class ImageTextureBackend
{
  public:
    virtual ~ImageTextureBackend() = default;

    /// Creates a texture and uploads @p param's pixels into it.
    ///
    /// The command is queued, so a failure surfaces long after this returns; the caller learns about it
    /// through @c takeFailedImageTextures().
    virtual void createImageTexture(CreateImageTexture param) = 0;

    /// Releases the texture. Ids of destroyed textures may be reused.
    virtual void destroyImageTexture(DestroyImageTexture param) = 0;

    /// Queues one quad sampling an image texture.
    virtual void renderImageQuad(RenderImageQuad param) = 0;

    /// Queues one solid fill, composited in issue order with the quads of the same side.
    virtual void renderImageGap(RenderImageGap param) = 0;

    /// Takes the ids whose creation failed since the last call, emptying the list.
    ///
    /// A texture the backend never created must not go on counting against the caller's budget, and
    /// nothing else would ever tell it: the caller commits an image to its cache before the queued
    /// creation runs, so without this it would hold a cache entry naming a texture that does not exist
    /// and never retry the upload.
    [[nodiscard]] virtual std::vector<ImageTextureId> takeFailedImageTextures() = 0;
};

} // namespace vtrasterizer::atlas
