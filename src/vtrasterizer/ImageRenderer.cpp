// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/ImageRenderer.h>

#include <crispy/StrongHash.h>
#include <crispy/algorithm.h>
#include <crispy/times.h>

#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <vector>

using crispy::times;

using std::array;
using std::nullopt;
using std::optional;

namespace vtrasterizer
{

ImageRenderer::ImageRenderer(GridMetrics const& gridMetrics, ImageSize cellSize, size_t textureBudgetBytes):
    Renderable { gridMetrics }, _cellSize { cellSize }, _textureBudgetBytes { textureBudgetBytes }
{
}

void ImageRenderer::setRenderTarget(RenderTarget& renderTarget,
                                    DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    clearCache();
}

void ImageRenderer::detachRenderTarget() noexcept
{
    // The textures belong to the render target and die with it, so every id naming one is worthless
    // from here on. Forgetting them is what keeps a later discard from routing at a scheduler that is
    // gone, and stops the budget being charged for textures that no longer exist.
    _imageTextures.clear();
    _residentBytes = 0;
    Renderable::detachRenderTarget();
}

void ImageRenderer::setCellSize(ImageSize cellSize)
{
    _cellSize = cellSize;
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

std::optional<atlas::ImageTextureId> ImageRenderer::textureFor(
    vtbackend::RasterizedImage const& rasterizedImage)
{
    // Keyed on the image, not on the rasterization: cell size and policies change where the image is
    // sampled, never what the texture holds.
    auto const& image = rasterizedImage.image();
    auto const imageId = image.id().value;
    if (auto const known = _imageTextures.find(imageId); known != _imageTextures.end())
    {
        known->second.lastUsedFrame = _frameCounter;
        return known->second.id;
    }

    // The upload hands the backend this geometry and lets it read height * width * 4 bytes from the
    // pixmap, so one that does not match would be read out of bounds. vtbackend rejects those where the
    // data enters; this restates the guarantee where the memory is actually indexed.
    if (!vtbackend::isConsistentPixmap(image.format(), image.size(), image.data().size()))
    {
        errorLog()("Refusing to upload image {}: {} pixmap of {} bytes does not match its size {}.",
                   imageId,
                   image.format(),
                   image.data().size(),
                   image.size());
        return std::nullopt;
    }

    auto const id = atlas::ImageTextureId { _nextImageTextureId++ };
    // Whatever the protocol sent, the texture is RGBA8 -- widenToRgba() below makes sure of it.
    auto const bytes = static_cast<size_t>(image.size().area()) * 4;
    _imageTextures.emplace(imageId,
                           ImageTextureEntry { .id = id, .bytes = bytes, .lastUsedFrame = _frameCounter });
    _residentBytes += bytes;

    // An already-RGBA pixmap uploads straight out of the Image's own buffer, holding the image alive
    // until the queued command executes. Copying it would cost a second full buffer (~33 MB for a 4K
    // image) on the render thread, every time an image is first seen.
    auto [pixels, owner] = [&]() -> std::pair<std::span<uint8_t const>, std::shared_ptr<void const>> {
        if (image.format() != vtbackend::ImageFormat::RGB)
            return { std::span<uint8_t const> { image.data() }, rasterizedImage.imagePointer() };
        auto widened = std::make_shared<vtbackend::Image::Data const>(widenToRgba(image.data()));
        return { std::span<uint8_t const> { *widened }, std::move(widened) };
    }();

    imageScheduler().createImageTexture(atlas::CreateImageTexture {
        .id = id,
        .size = image.size(),
        .format = atlas::Format::RGBA,
        .data = pixels,
        .owner = std::move(owner),
    });
    evictToBudget();
    return id;
}

void ImageRenderer::dropFailedTextures()
{
    if (!renderTargetAvailable())
        return;

    for (auto const failedId: imageScheduler().takeFailedImageTextures())
    {
        auto const entry = std::ranges::find_if(
            _imageTextures, [failedId](auto const& kv) { return kv.second.id == failedId; });
        if (entry == _imageTextures.end())
            continue;
        // The texture was never created, so it may not go on charging the budget -- and forgetting the
        // entry is what lets the next sight of the image attempt the upload again.
        _residentBytes -= entry->second.bytes;
        _imageTextures.erase(entry);
    }
}

void ImageRenderer::evictToBudget()
{
    if (_residentBytes <= _textureBudgetBytes)
        return;

    // Only what this frame did not draw may go; see the declaration. Carrying the LRU key alongside the
    // id keeps the sort from hashing its way back into the map on every comparison.
    auto candidates = std::vector<std::pair<uint64_t, uint32_t>> {};
    for (auto const& [imageId, entry]: _imageTextures)
        if (entry.lastUsedFrame != _frameCounter)
            candidates.emplace_back(entry.lastUsedFrame, imageId);

    std::ranges::sort(candidates);

    for (auto const& [lastUsedFrame, imageId]: candidates)
    {
        if (_residentBytes <= _textureBudgetBytes)
            break;
        // Exactly what a pool removal does: release the texture and forget the mapping, so the next
        // sight of this image uploads it again.
        discardImage(vtbackend::ImageId { imageId });
    }
}

void ImageRenderer::renderImage(crispy::point pos, vtbackend::ImageFragment const& fragment)
{
    auto const& rasterizedImage = fragment.rasterizedImage();
    auto const placement = rasterizedImage.fragmentPlacement(fragment.offset(), _cellSize);
    if (!placement.hasImage)
        return; // the cell lies wholly in the alignment gap

    auto const texture = textureFor(rasterizedImage);
    if (!texture)
        return; // the image has no texture to sample and never will; drawing it is not possible

    auto const quad = atlas::RenderImageQuad {
        .texture = *texture,
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
    // Stamps what textureFor() touches from here on. That is what tells an image this frame draws --
    // whose quads already name its texture, so it may not be released -- from one merely resident.
    // Advanced once per frame rather than once per pass: a second pass under a later stamp would offer
    // up the images the first pass had already scheduled, and releasing one drops it from the frame it
    // is visible in.
    ++_frameCounter;

    dropFailedTextures();
}

void ImageRenderer::beginPass()
{
    if (!SoftRequire(_pendingQuadsBelowText.empty()))
        _pendingQuadsBelowText.clear();
    if (!SoftRequire(_pendingQuadsAboveText.empty()))
        _pendingQuadsAboveText.clear();
}

void ImageRenderer::endPass()
{
    // Flush anything the text pass did not pick up (a pass with images but no text).
    flushQuads(_pendingQuadsBelowText);
    flushQuads(_pendingQuadsAboveText);
}

void ImageRenderer::discardImage(vtbackend::ImageId imageId)
{
    auto const known = _imageTextures.find(imageId.value);
    if (known == _imageTextures.end())
        return;
    imageScheduler().destroyImageTexture(atlas::DestroyImageTexture { .id = known->second.id });
    _residentBytes -= known->second.bytes;
    _imageTextures.erase(known);
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
    for (auto const& [imageId, entry]: _imageTextures)
        imageScheduler().destroyImageTexture(atlas::DestroyImageTexture { .id = entry.id });
    _imageTextures.clear();
    _residentBytes = 0;
}

void ImageRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
