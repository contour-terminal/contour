// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/ImageRenderer.h>

#include <crispy/StrongHash.h>
#include <crispy/algorithm.h>
#include <crispy/times.h>

#include <array>
#include <ranges>
#include <span>

using crispy::times;

using std::array;
using std::nullopt;
using std::optional;

namespace vtrasterizer
{

ImageRenderer::ImageRenderer(GridMetrics const& gridMetrics, ImageSize cellSize):
    Renderable { gridMetrics }, _cellSize { cellSize }
{
}

void ImageRenderer::setRenderTarget(RenderTarget& renderTarget,
                                    DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    clearCache();
}

void ImageRenderer::setCellSize(ImageSize cellSize)
{
    _cellSize = cellSize;
    // TODO: recompute rasterized images slices here?
}

namespace
{
    /// Widens packed 24-bit RGB to 32-bit RGBA with an opaque alpha.
    ///
    /// An Image keeps whatever the protocol sent -- GIP's `f=2` really is three bytes per pixel --
    /// while the GPU wants a four-byte format (RGB8 is not a texture format the RHI can rely on).
    /// @param rgb Packed RGB, three bytes per pixel.
    /// @return The same pixels as RGBA, four bytes per pixel.
    [[nodiscard]] vtbackend::Image::Data widenToRgba(std::span<uint8_t const> rgb)
    {
        auto rgba = vtbackend::Image::Data(rgb.size() / 3 * 4);
        for (auto const pixel: std::views::iota(size_t { 0 }, rgb.size() / 3))
        {
            rgba[(pixel * 4) + 0] = rgb[(pixel * 3) + 0];
            rgba[(pixel * 4) + 1] = rgb[(pixel * 3) + 1];
            rgba[(pixel * 4) + 2] = rgb[(pixel * 3) + 2];
            rgba[(pixel * 4) + 3] = 0xFF;
        }
        return rgba;
    }
} // namespace

atlas::ImageTextureId ImageRenderer::textureFor(vtbackend::RasterizedImage const& rasterizedImage)
{
    // Keyed on the image, not on the rasterization: cell size and policies change where the image is
    // sampled, never what the texture holds.
    auto const& image = rasterizedImage.image();
    auto const imageId = image.id().value;
    if (auto const known = _imageTextureIds.find(imageId); known != _imageTextureIds.end())
        return known->second;

    auto const id = atlas::ImageTextureId { _nextImageTextureId++ };
    _imageTextureIds.emplace(imageId, id);
    imageScheduler().createImageTexture(atlas::CreateImageTexture {
        .id = id,
        .size = image.size(),
        .format = atlas::Format::RGBA,
        .data = image.format() == vtbackend::ImageFormat::RGB ? widenToRgba(image.data()) : image.data(),
    });
    return id;
}

void ImageRenderer::renderImage(crispy::point pos, vtbackend::ImageFragment const& fragment)
{
    auto const& rasterizedImage = fragment.rasterizedImage();
    auto const placement = rasterizedImage.fragmentPlacement(fragment.offset(), _cellSize);
    if (!placement.hasImage)
        return; // the cell lies wholly in the alignment gap

    auto const quad = atlas::RenderImageQuad {
        .texture = textureFor(rasterizedImage),
        .x = pos.x + placement.targetX,
        .y = pos.y + placement.targetY,
        .targetSize = placement.targetSize,
        .source = atlas::NormalizedTileLocation { .x = placement.sourceX,
                                                  .y = placement.sourceY,
                                                  .width = placement.sourceWidth,
                                                  .height = placement.sourceHeight },
        .color = { 1.0f, 1.0f, 1.0f, 1.0f },
        .aboveText = rasterizedImage.layer() != vtbackend::ImageLayer::Below,
    };

    // Deliberately mirrors the previous routing, including that ImageLayer::Replace lands above the
    // text: this change is about how an image reaches the GPU, not about what composites over what.
    if (quad.aboveText)
        _pendingQuadsAboveText.emplace_back(quad);
    else
        _pendingQuadsBelowText.emplace_back(quad);
}

void ImageRenderer::flushQuads(std::vector<atlas::RenderImageQuad>& quads)
{
    // Quads arrive per cell and in order, so a run along one line is contiguous in this vector.
    // Coalescing is left to the backend, which is where the run becomes a draw call.
    for (auto const& quad: quads)
        imageScheduler().renderImageQuad(quad);
    quads.clear();
}

void ImageRenderer::onBeforeRenderingText()
{
    flushQuads(_pendingQuadsBelowText);
}

void ImageRenderer::onAfterRenderingText()
{
    flushQuads(_pendingQuadsAboveText);
}

void ImageRenderer::beginFrame()
{
    if (!SoftRequire(_pendingQuadsBelowText.empty()))
        _pendingQuadsBelowText.clear();
    if (!SoftRequire(_pendingQuadsAboveText.empty()))
        _pendingQuadsAboveText.clear();
}

void ImageRenderer::endFrame()
{
    // Flush anything the text pass did not pick up (a frame with images but no text).
    flushQuads(_pendingQuadsBelowText);
    flushQuads(_pendingQuadsAboveText);
}

void ImageRenderer::discardImage(vtbackend::ImageId imageId)
{
    auto const known = _imageTextureIds.find(imageId.value);
    if (known == _imageTextureIds.end())
        return;
    imageScheduler().destroyImageTexture(atlas::DestroyImageTexture { .id = known->second });
    _imageTextureIds.erase(known);
}

void ImageRenderer::clearCache()
{
    // Release the textures, not merely the mapping that names them.
    //
    // "The textures belong to the render target, so a new one starts with none of them" is only true
    // of the setRenderTarget() caller. The other one, Renderer::updateFontMetrics(), clears on a
    // render target that is still very much alive: dropping the ids alone stranded one whole-image
    // texture per cached image on the GPU with nothing left able to name it, so every font-size step
    // leaked the full resident set and re-uploaded it.
    //
    // Issuing the destroys is right for the setRenderTarget() caller too: they reach a target that
    // never had these ids, whose backend ignores them, while the textures of the target being
    // replaced die with it.
    for (auto const& [imageId, textureId]: _imageTextureIds)
        imageScheduler().destroyImageTexture(atlas::DestroyImageTexture { .id = textureId });
    _imageTextureIds.clear();
}

void ImageRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
