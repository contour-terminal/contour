// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtrasterizer/TextureAtlas.h>

#include <array>
#include <cstdint>

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
    Buffer data;
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
    virtual void createImageTexture(CreateImageTexture param) = 0;

    /// Releases the texture. Ids of destroyed textures may be reused.
    virtual void destroyImageTexture(DestroyImageTexture param) = 0;

    /// Queues one quad sampling an image texture.
    virtual void renderImageQuad(RenderImageQuad param) = 0;
};

} // namespace vtrasterizer::atlas
