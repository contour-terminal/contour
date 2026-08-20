// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/Renderer.hpp>

#include <vtrasterizer/AtlasBudget.hpp>
#include <vtrasterizer/TextRenderer.hpp>
#include <vtrasterizer/Utils.hpp>

#include <text_shaper/FontLocator.hpp>
#include <text_shaper/OpenShaper.hpp>

#include <crispy/StrongLRUHashtable.hpp>
#include <crispy/Utils.hpp>

#ifdef _WIN32
    #include <text_shaper/DirectWriteShaper.hpp>
#endif

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>

using std::initializer_list;
using std::make_unique;
using std::optional;
using std::scoped_lock;
using std::unique_ptr;
using std::vector;

namespace vtrasterizer
{

namespace
{

    void loadGridMetricsFromFont(text::FontKey font, GridMetrics& gm, text::Shaper& textShaper)
    {
        auto const m = textShaper.metrics(font);

        // The font's own advance, unrounded. Snapping this to a whole logical pixel -- so that the
        // cell size could be reported to applications exactly -- was tried and reverted: at a
        // fractional content scale the quantum is the scale's numerator, so at 1.75 (= 7/4) the cell
        // must be a multiple of 7 device pixels and a 17px advance becomes 21. That is 4px of dead
        // space on every column, and it looks it. Text spacing is not negotiable for the sake of an
        // image canvas measured in whole cells.
        gm.cellSize.width = vtbackend::Width::cast_from(m.advance);
        gm.cellSize.height = vtbackend::Height::cast_from(m.lineHeight);
        gm.baseline = m.lineHeight - m.ascender;
        gm.underline.position = gm.baseline + m.underlinePosition;
        gm.underline.thickness = m.underlineThickness;

        rendererLog()("Loading grid metrics {}", gm);
    }

    GridMetrics loadGridMetrics(text::FontKey font,
                                vtbackend::PageSize pageSize,
                                PageMargin pageMargin,
                                text::Shaper& textShaper)
    {
        auto gm = GridMetrics {};

        gm.pageSize = pageSize;
        gm.cellMargin = {
            .top = 0, .left = 0, .bottom = 0, .right = 0
        }; // TODO (pass as args, and make use of them)
        // The configured origin, not zero: applyResize() refines it against the real surface, but
        // until it runs this is what every consumer of gridMetrics() maps through.
        gm.pageMargin = pageMargin;

        loadGridMetricsFromFont(font, gm, textShaper);

        return gm;
    }

    /// Loads the font keys (regular/bold/italic/...) for the given descriptions.
    ///
    /// @throws std::runtime_error if the @e regular font fails to load. The regular key is the
    ///         anchor every other style falls back to, and a default-constructed (invalid) FontKey
    ///         would later abort inside text::Shaper::metrics() (Require on the key mapping). Throwing
    ///         instead lets applyPendingReconfig()'s try/catch keep the previously loaded font, honoring
    ///         the "keep previous font on failure" guarantee, and surfaces a startup font-load failure
    ///         as a clear error rather than a deep abort.
    FontKeys loadFontKeys(FontDescriptions const& fd, text::Shaper& shaper)
    {
        FontKeys output {};
        auto const regularOpt = shaper.loadFont(fd.regular, fd.size);
        if (!regularOpt.has_value())
            throw std::runtime_error(std::format("Failed to load regular font: {}", fd.regular));
        output.regular = regularOpt.value();
        output.bold = shaper.loadFont(fd.bold, fd.size).value_or(output.regular);
        output.italic = shaper.loadFont(fd.italic, fd.size).value_or(output.regular);
        output.boldItalic = shaper.loadFont(fd.boldItalic, fd.size).value_or(output.regular);
        output.emoji = shaper.loadFont(fd.emoji, fd.size).value_or(output.regular);

        return output;
    }

    unique_ptr<text::Shaper> createTextShaper(TextShapingEngine engine, DPI dpi, text::FontLocator& locator)
    {
        switch (engine)
        {
            case TextShapingEngine::DWrite:
#ifdef _WIN32
                rendererLog()("Using DirectWrite text shaping engine.");
                // TODO: do we want to use custom font locator here?
                return make_unique<text::DirectWriteShaper>(dpi, locator);
#else
                rendererLog()("DirectWrite not available on this platform.");
                break;
#endif

            case TextShapingEngine::CoreText:
#ifdef __APPLE__
                rendererLog()("CoreText not yet implemented.");
                break;
#else
                rendererLog()("CoreText not available on this platform.");
                break;
#endif

            case TextShapingEngine::OpenShaper: break;
        }

        rendererLog()("Using OpenShaper text shaping engine.");
        return make_unique<text::OpenShaper>(dpi, locator);
    }

} // namespace

Renderer::Renderer(vtbackend::PageSize pageSize,
                   PageMargin pageMargin,
                   FontDescriptions fontDescriptions,
                   vtbackend::ColorPalette const& colorPalette,
                   crispy::StrongHashtableSize atlasHashtableSlotCount,
                   crispy::LRUCapacity atlasTileCount,
                   bool atlasDirectMapping,
                   text::FontLocator& fontLocator,
                   Decorator hyperlinkNormal,
                   Decorator hyperlinkHover,
                   GlyphScalingMethod textScalingMethod):
    _configuredAtlasHashtableSlotCount { atlasHashtableSlotCount },
    _configuredAtlasTileCount { atlasTileCount },
    _atlasHashtableSlotCount { atlasbudget::slotCountFor(
        atlasHashtableSlotCount, atlasbudget::tileCountFor(atlasTileCount, pageSize)) },
    _atlasTileCount { atlasbudget::tileCountFor(atlasTileCount, pageSize) },
    _atlasDirectMapping { atlasDirectMapping },
    //.
    _fontLocator { fontLocator },
    _fontDescriptions { std::move(fontDescriptions) },
    _textShaper { [&] {
        auto shaper =
            createTextShaper(_fontDescriptions.textShapingEngine, _fontDescriptions.dpi, fontLocator);
        shaper->setFontFallbackLimit(_fontDescriptions.maxFallbackCount);
        return shaper;
    }() },
    _fonts { loadFontKeys(_fontDescriptions, *_textShaper) },
    _gridMetrics { loadGridMetrics(_fonts.regular, pageSize, pageMargin, *_textShaper) },
    _publishedMetrics { _gridMetrics },
    _publishedFontDescriptions { _fontDescriptions },
    _publishedCellSize { _gridMetrics.cellSize },
    //.
    _backgroundRenderer { _gridMetrics, colorPalette.defaultBackground },
    _imageRenderer { _gridMetrics, cellSize() },
    _textRenderer { _gridMetrics, *_textShaper,   _fontDescriptions,
                    _fonts,       _imageRenderer, glyphScalerFor(textScalingMethod) },
    _decorationRenderer { _gridMetrics, hyperlinkNormal, hyperlinkHover },
    _cursorRenderer { _gridMetrics, vtbackend::CursorShape::Block }
{
    _textRenderer.updateFontMetrics();
    _imageRenderer.setCellSize(cellSize());

    // clang-format off
    if (_atlasTileCount.value > atlasTileCount.value)
        rendererLog()("Increasing atlas tile count configuration to {} to satisfy worst-case rendering scenario.",
                              _atlasTileCount.value);

    if (_atlasHashtableSlotCount.value > atlasHashtableSlotCount.value)
        rendererLog()("Increasing atlas hashtable slot count configuration to the next power of two: {}.",
                              _atlasHashtableSlotCount.value);
    // clang-format on
}

void Renderer::setRenderTarget(RenderTarget& renderTarget)
{
    _renderTarget = &renderTarget;

    // Reset DirectMappingAllocator (also skipping zero-tile).
    _directMappingAllocator =
        atlas::DirectMappingAllocator<RenderTileAttributes> { .currentlyAllocatedCount = 1 };

    // Explicitly enable direct mapping for everything BUT the text renderer.
    // Only the text renderer's direct mapping is configurable (for simplicity for now).
    _directMappingAllocator.enabled = true;
    for (Renderable* renderable: initializer_list<Renderable*> {
             &_backgroundRenderer, &_cursorRenderer, &_decorationRenderer, &_imageRenderer })
        renderable->setRenderTarget(renderTarget, _directMappingAllocator);
    _directMappingAllocator.enabled = _atlasDirectMapping;
    _textRenderer.setRenderTarget(renderTarget, _directMappingAllocator);

    renderTarget.setTextOutline(_fontDescriptions.textOutline.thickness, _fontDescriptions.textOutline.color);

    configureTextureAtlas();
}

void Renderer::detachRenderTarget() noexcept
{
    _renderTarget = nullptr;
    // The atlas references the dead target's texture scheduler; drop it with the target. The next
    // setRenderTarget() rebuilds it (configureTextureAtlas) and re-points every renderable.
    _textureAtlas.reset();
    for (gsl::not_null<Renderable*> const& renderable: renderables())
        renderable->detachRenderTarget();
}

crispy::LRUCapacity Renderer::boundedAtlasTileCount(crispy::LRUCapacity wanted) const noexcept
{
    auto const ceiling = atlasbudget::maxTileCountFor(_gridMetrics.cellSize,
                                                      _directMappingAllocator.currentlyAllocatedCount,
                                                      atlasbudget::MaxAtlasTextureEdge);
    return crispy::LRUCapacity { std::min(wanted.value, ceiling.value) };
}

void Renderer::configureTextureAtlas()
{
    Require(_renderTarget);

    auto const atlasCellSize = _gridMetrics.cellSize;

    // Clamped HERE because this is the one place a budget becomes a texture, so every path into it --
    // construction, a font or DPI change, page growth -- is bounded, and afterwards the member states
    // what the atlas actually holds rather than what the page asked for. Losing tiles this way brings
    // back the mid-frame recycling the budget exists to prevent, so it is reported at every rebuild:
    // a texture the driver refuses to allocate would lose the glyphs entirely and say nothing.
    if (auto const bounded = boundedAtlasTileCount(_atlasTileCount); bounded.value < _atlasTileCount.value)
    {
        rendererLog()("Capping atlas tile budget at {} (page asked for {}): {} tiles of {} do not fit a "
                      "texture of at most {}x{} pixels.",
                      bounded.value,
                      _atlasTileCount.value,
                      _atlasTileCount.value,
                      atlasCellSize,
                      atlasbudget::MaxAtlasTextureEdge,
                      atlasbudget::MaxAtlasTextureEdge);
        _atlasTileCount = bounded;
        _atlasHashtableSlotCount = atlasbudget::slotCountFor(_configuredAtlasHashtableSlotCount, bounded);
    }

    auto atlasProperties =
        atlas::AtlasProperties { .format = atlas::Format::RGBA,
                                 .tileSize = atlasCellSize,
                                 .hashCount = _atlasHashtableSlotCount,
                                 .tileCount = _atlasTileCount,
                                 .directMappingCount = _directMappingAllocator.currentlyAllocatedCount };

    Require(atlasProperties.tileCount.value > 0);

    _textureAtlas = make_unique<Renderable::TextureAtlas>(_renderTarget->textureScheduler(), atlasProperties);

    // clang-format off
    rendererLog()("Configuring texture atlas.\n", atlasProperties);
    rendererLog()("- Atlas properties     : {}\n", atlasProperties);
    rendererLog()("- Atlas texture size   : {} pixels\n", _textureAtlas->atlasSize());
    rendererLog()("- Atlas hashtable      : {} slots\n", _atlasHashtableSlotCount.value);
    rendererLog()("- Atlas tile count     : {} = {}x * {}y\n", _textureAtlas->capacity(), _textureAtlas->tilesInX(), _textureAtlas->tilesInY());
    rendererLog()("- Atlas direct mapping : {} (for text rendering)", _atlasDirectMapping ? "enabled" : "disabled");
    // clang-format on

    for (gsl::not_null<Renderable*> const& renderable: renderables())
        renderable->setTextureAtlas(*_textureAtlas);
}

void Renderer::growAtlasForPage(vtbackend::PageSize pageSize)
{
    // The budget was previously fixed at construction, from the page size the renderer happened to start
    // with. A pane created small — which a freshly split pane always is — and then grown by a window
    // resize kept the small-pane atlas for the rest of its life, and once a frame needed more distinct
    // tiles than that atlas held, tile slots were recycled mid-frame and glyphs rendered as each other.
    // Quantized to the atlas' own power-of-two granularity and clamped to what the GPU can hold, both
    // so that this converges: an enlarging resize drag walks the page one cell at a time, and an
    // un-quantized budget would clear the guard below on nearly every step, rebuilding a multi-megabyte
    // texture and re-uploading every sixel image once per frame of the drag. Every budget inside one
    // band produces the same texture anyway, so snapping to the top of the band costs nothing.
    auto const normalize = [this](crispy::LRUCapacity budget) {
        return boundedAtlasTileCount(
            atlasbudget::quantizedTileCountFor(budget, _directMappingAllocator.currentlyAllocatedCount));
    };

    // Both sides go through the same normalization, so the constructor's un-normalized starting budget
    // and every later one are compared on one grid -- otherwise the first resize after construction
    // would always appear to be a growth.
    auto const required = normalize(atlasbudget::tileCountFor(_configuredAtlasTileCount, pageSize));
    if (required.value <= normalize(_atlasTileCount).value)
        return; // grow-only: resize jitter must not churn the atlas every frame, and surplus only costs VRAM

    rendererLog()("Growing atlas tile budget to {} for page size {}.", required.value, pageSize);

    if (!_renderTarget)
    {
        // No atlas to rebuild yet; setRenderTarget() will build it at the new budget.
        _atlasTileCount = required;
        _atlasHashtableSlotCount = atlasbudget::slotCountFor(_configuredAtlasHashtableSlotCount, required);
        return;
    }

    // configureTextureAtlas() replaces the atlas wholesale, and a tile's normalized location was baked
    // into vertex data against the OLD atlas, so every cache holding one must go with it — the same
    // pairing updateFontMetrics() uses.
    //
    // The budget is committed only once that rebuild has returned. Atlas (re)allocation can throw --
    // applyPendingReconfig()'s font branch says so and wraps itself accordingly, while this geometry
    // branch does not -- and raising the member first would make the guard above swallow every later
    // attempt, stranding the pane on the undersized atlas for good.
    auto const previousTileCount = _atlasTileCount;
    auto const previousSlotCount = _atlasHashtableSlotCount;
    _atlasTileCount = required;
    _atlasHashtableSlotCount = atlasbudget::slotCountFor(_configuredAtlasHashtableSlotCount, required);
    try
    {
        configureTextureAtlas();
    }
    catch (...)
    {
        _atlasTileCount = previousTileCount;
        _atlasHashtableSlotCount = previousSlotCount;
        throw;
    }
    clearCache();
}

void Renderer::discardImage(vtbackend::Image const& image)
{
    // Defer rendering into the renderer thread & render stage, as this call might have
    // been coming out of bounds from another thread (e.g. the terminal's screen update thread)
    auto l = scoped_lock { _imageDiscardLock };
    _discardImageQueue.emplace_back(image.id());
}

void Renderer::executeImageDiscards()
{
    auto l = scoped_lock { _imageDiscardLock };

    for (auto const imageId: _discardImageQueue)
        _imageRenderer.discardImage(imageId);

    _discardImageQueue.clear();
}

void Renderer::clearCache()
{
    // Only the render target's own cache needs a target; every renderable's cache is CPU-side. Skipping
    // the whole flush when detached left the shaping cache holding results that named fonts the caller
    // had just invalidated — a pane that is occluded, minimized or between detachRenderTarget() and the
    // next setRenderTarget() is exactly that case.
    if (_renderTarget)
        _renderTarget->clearCache();

    // TODO(?): below functions are actually doing the same again and again and again. delete them (and their
    // functions for that) either that, or only the render target is allowed to clear the actual atlas caches.
    for (auto& renderable: renderables())
        renderable->clearCache();
}

void Renderer::setFonts(FontDescriptions fontDescriptions)
{
    // The text shaper is not thread-safe and is used by the render thread during every frame, so we
    // only *stage* the requested font descriptions here (on the UI thread). The shaper reconfiguration,
    // font loading and grid-metrics/atlas rebuild happen on the render thread in applyPendingReconfig().
    auto const l = std::scoped_lock { _reconfigMutex };
    auto& pending = ensurePendingLocked();
    pending.fontDescriptions = std::move(fontDescriptions);
    // A full font-descriptions change supersedes any pending size-only change.
    pending.fontSize.reset();
}

void Renderer::setFontDPI(DPI dpi)
{
    auto const l = std::scoped_lock { _reconfigMutex };

    // Determine the *effective* descriptions WITHOUT creating a pending reconfig yet: an already-staged
    // font-descriptions change (so a concurrent font-family change isn't clobbered), otherwise the
    // published snapshot. We must NOT read the live _fontDescriptions here: it is mutated on the render
    // thread (applyPendingReconfig) without _reconfigMutex, so reading it would be a data race. The
    // published snapshot is render-thread-published under this same mutex. Bail out before staging
    // anything if the DPI is unchanged, so an unchanged-DPI call does not spuriously stage an (empty)
    // reconfiguration.
    auto const& effective = (_pendingReconfig && _pendingReconfig->fontDescriptions)
                                ? *_pendingReconfig->fontDescriptions
                                : _publishedFontDescriptions;
    if (effective.dpi == dpi)
        return;

    auto descriptions = effective;
    descriptions.dpi = dpi;

    auto& pending = ensurePendingLocked();

    // Promoting a size-only change to a full descriptions change would otherwise drop the staged size
    // (applyPendingReconfig() applies fontDescriptions XOR fontSize), so fold it in before clearing it.
    if (pending.fontSize)
    {
        descriptions.size = *pending.fontSize;
        pending.fontSize.reset();
    }

    pending.fontDescriptions = std::move(descriptions);
}

void Renderer::applyFontDescriptions(FontDescriptions fontDescriptions)
{
    if (fontDescriptions == _fontDescriptions)
        return;

    // When only DPI changed, the enhanced setDPI() updates existing FT_Face
    // objects in-place. Skip clearCache() to avoid destroying them.
    auto descriptionsWithSameDpi = fontDescriptions;
    descriptionsWithSameDpi.dpi = _fontDescriptions.dpi;
    auto const onlyDpiChanged = (descriptionsWithSameDpi == _fontDescriptions);

    // Said out loud rather than dropped on the floor: neither branch below can honour a new
    // FontLocatorEngine -- the locator is constructor-injected and both the same-engine path and the
    // replacement shaper resolve through _fontLocator. Committing _fontDescriptions further down then
    // makes the configuration model report the new engine while the fonts on screen keep coming from
    // the old one, and without this line the only symptom is "my font_locator setting does nothing".
    if (_fontDescriptions.fontLocator != fontDescriptions.fontLocator)
        rendererLog()("Ignoring font locator change to {}: the locator is fixed for this renderer's "
                      "lifetime; restart to apply it.",
                      fontDescriptions.fontLocator);

    // A replacement shaper, when the engine changed. Built and loaded into BEFORE anything is
    // committed, and only then swapped in: loadFontKeys() throws whenever the regular font will not
    // load, and installing the new shaper first left the renderer holding it alongside FontKeys issued
    // by the OLD one. applyPendingReconfig()'s try/catch then reports "keeping the previous font" over a
    // renderer that can no longer shape at all -- every lookup is a miss in the new shaper's font map,
    // which throws from inside the frame. Windows CI caught exactly that: DirectWrite cannot load the
    // BDF test font, so the switch fails there on every attempt.
    auto replacementShaper = unique_ptr<text::Shaper> {};

    if (_fontDescriptions.textShapingEngine == fontDescriptions.textShapingEngine)
    {
        // Mutating the shaper we already have is safe to have run on a failed attempt: it is
        // render-thread-owned, and re-applying the same DPI and fallback limit is idempotent.
        if (!onlyDpiChanged)
            _textShaper->clearCache();
        _textShaper->setDPI(fontDescriptions.dpi);
        _textShaper->setFontFallbackLimit(fontDescriptions.maxFallbackCount);
    }
    else
    {
        replacementShaper =
            createTextShaper(fontDescriptions.textShapingEngine, fontDescriptions.dpi, _fontLocator);
        replacementShaper->setFontFallbackLimit(fontDescriptions.maxFallbackCount);
    }

    // Load the fonts against the NEW descriptions, through whichever shaper is going to serve them, but
    // only commit once the load succeeds. loadFontKeys()/updateFontMetrics() (atlas reconfiguration) can
    // throw, and this is called from applyPendingReconfig()'s try/catch which keeps the previous font on
    // failure. Committing _fontDescriptions first (as before) would leave it at a never-loaded value: the
    // change-detection guard at the top of this function and helper.cpp::applyFontDescription would then
    // both early-return on retry, permanently stranding the wrong font. This mirrors the size-only branch
    // in applyPendingReconfig().
    auto fonts = loadFontKeys(fontDescriptions, replacementShaper ? *replacementShaper : *_textShaper);

    if (replacementShaper)
    {
        // The assignment destroys the shaper _textRenderer was constructed against, and TextRenderer
        // holds it for the whole frame path (shape, resizeFont, rasterize). Without the rebind every
        // draw after an engine switch runs through freed memory. The FontKeys above and the
        // updateFontMetrics() below supply the other half setTextShaper() requires: fresh keys, and
        // caches emptied of everything naming the old shaper's glyphs.
        _textShaper = std::move(replacementShaper);
        _textRenderer.setTextShaper(*_textShaper);
    }

    _fonts = fonts;
    _fontDescriptions = std::move(fontDescriptions);
    updateFontMetrics();

    if (_renderTarget)
        _renderTarget->setTextOutline(_fontDescriptions.textOutline.thickness,
                                      _fontDescriptions.textOutline.color);
}

bool Renderer::setFontSize(text::FontSize fontSize)
{
    if (fontSize.pt < 5.) // Let's not be crazy.
        return false;

    if (fontSize.pt > 200.)
        return false;

    // The text shaper is not thread-safe and is used by the render thread during every frame.
    // We therefore only *stage* the requested size here (on the UI thread); the actual font loading
    // and grid-metrics recomputation happen on the render thread in applyPendingReconfig().
    auto const l = std::scoped_lock { _reconfigMutex };
    auto& pending = ensurePendingLocked();
    if (pending.fontDescriptions)
        // A full font-descriptions change is already staged; fold the new size into it so the most
        // recent size wins rather than being dropped when the descriptions are applied.
        pending.fontDescriptions->size = fontSize;
    else
        pending.fontSize = fontSize;

    return true;
}

void Renderer::updateFontMetrics()
{
    rendererLog()("Updating grid metrics: {}", _gridMetrics);

    // Update only the font-derived fields (cellSize, baseline, underline) in place. Page geometry
    // (pageSize, pageMargin, cellMargin) is owned by the geometry path, not the font, so leave it
    // untouched — this is what lets callers avoid manually saving/restoring it around a font rebuild.
    loadGridMetricsFromFont(_fonts.regular, _gridMetrics, *_textShaper);

    if (_renderTarget)
        configureTextureAtlas();

    _textRenderer.updateFontMetrics();
    _imageRenderer.setCellSize(cellSize());

    clearCache();
}

void Renderer::applyPendingReconfig()
{
    // Mutates _gridMetrics / the texture atlas / the non-thread-safe shaper. The caller must hold
    // _applyMutex for the duration that those are also *read* — on the render thread that is the whole
    // renderImpl() (it renders from _gridMetrics after this returns); on the GUI thread's not-renderable
    // path applyStagedReconfigDuringSetup() holds it across this call. That serialization is what keeps
    // a GUI-thread apply from overlapping an in-flight render-thread frame (a window losing exposure
    // does not synchronously stop one).

    // Fast path: avoid touching _pendingReconfig on every frame when nothing is staged. The atomic is
    // set under _reconfigMutex by the UI-thread writers, so a false reading here means no writer has
    // committed a request yet — and the next frame will pick it up.
    if (!_reconfigPending.load(std::memory_order_relaxed))
        return;

    // Take the pending request under the lock, then release it before the heavyweight apply below.
    // _gridMetrics, _fonts and the (non-thread-safe) text shaper are only ever touched on the render
    // thread, so they need no lock; only _pendingReconfig/_publishedMetrics are shared. Holding the
    // lock across loadFontKeys()/configureTextureAtlas() (multi-millisecond font loading + atlas
    // rebuild) would block every UI-thread gridMetrics() reader for that whole duration.
    auto pendingOpt = std::optional<PendingReconfig> {};
    {
        auto const l = std::scoped_lock { _reconfigMutex };
        if (!_pendingReconfig)
            return;
        pendingOpt = std::move(*_pendingReconfig);
        _pendingReconfig.reset();
        _reconfigPending.store(false, std::memory_order_relaxed);
    }
    auto const& pending = *pendingOpt;

    // Apply a pending geometry change (page size, margin, render-surface pixel size).
    if (pending.geometry)
    {
        auto const& geometry = *pending.geometry;
        if (_renderTarget)
        {
            if (geometry.pixelSize)
                _renderTarget->setRenderSize(*geometry.pixelSize);
            _renderTarget->setMargin(geometry.pageMargin);
        }
        _gridMetrics.pageSize = geometry.pageSize;
        _gridMetrics.pageMargin = geometry.pageMargin;
        growAtlasForPage(geometry.pageSize);
    }

    // Apply a pending font change (full descriptions or size-only): reconfigure the shaper, load the
    // fonts and rebuild grid metrics + atlas here, on the render thread, so the non-thread-safe text
    // shaper is never touched concurrently with rendering.
    if (pending.fontDescriptions || pending.fontSize)
    {
        // Remember the cell size before the apply so we can tell whether the font change actually
        // altered the cell metrics. A no-op font apply (e.g. setFontSize() with the current size, or a
        // config reload re-applying identical fonts) must NOT signal a font reconfig: doing so triggers
        // a redundant, terminal-locked resize round-trip on the UI thread for a cell size that did not
        // change (the symmetric counterpart to the geometry-only case guarded above).
        auto const cellSizeBefore = _gridMetrics.cellSize;

        // updateFontMetrics() now updates only the font-derived fields in place and leaves page geometry
        // (pageSize/pageMargin/cellMargin) untouched, so no manual save/restore around the rebuild is
        // needed — the font rebuild and page geometry are decoupled.
        //
        // Font loading (FreeType face creation, file I/O) and atlas (re)allocation can throw. The
        // request has already been consumed, so a throw here would otherwise be swallowed by render()'s
        // catch-all and silently lost. The underlying loadFontKeys()/configureTextureAtlas() report
        // failure by throwing rather than via std::expected, so we catch locally, log with context, and
        // fall through to re-publish the (unchanged) metrics without signalling a font reconfig — the
        // display must not resize against half-applied metrics.
        auto applied = false;
        try
        {
            if (pending.fontDescriptions)
            {
                applyFontDescriptions(*pending.fontDescriptions);
            }
            else // pending.fontSize
            {
                // Stage the size into a copy first; only commit it to _fontDescriptions once the font
                // actually loaded, so a load failure does not leave _fontDescriptions.size at a value
                // whose glyphs were never loaded (which would corrupt later change-detection and the
                // size reported to the UI).
                auto descriptions = _fontDescriptions;
                descriptions.size = *pending.fontSize;
                _fonts = loadFontKeys(descriptions, *_textShaper);
                _fontDescriptions = std::move(descriptions);
                updateFontMetrics();
            }
            applied = true;
        }
        catch (std::exception const& e)
        {
            errorLog()("Failed to apply staged font reconfiguration: {}. Keeping previous font.", e.what());
            // Fall through to the publish below with applied == false: readers stay consistent with the
            // (unchanged) live metrics, and no font reconfig is signalled.
        }

        // Signal the display to recompute the page size against the new cell size (see
        // consumeFontReconfigApplied()) only if the apply succeeded AND the cell size actually changed:
        // the grid dimensions derived on the UI thread before this apply are stale only then.
        if (applied && _gridMetrics.cellSize != cellSizeBefore)
            _fontReconfigApplied.store(true, std::memory_order_release);

        publishFontMetricsAndDescriptions();
    }
}

void Renderer::publishFontMetricsAndDescriptions()
{
    // Publish the render-thread-owned (font-derived) metrics and the font descriptions so UI-thread
    // readers (gridMetrics(), publishedCellSize(), fontDescriptions()) observe them. Re-acquire the lock
    // only for this short copy, not across the font load/atlas rebuild that precedes the call.
    //
    // Publish only the render-thread-owned (font-derived) fields. pageSize/pageMargin are owned by the
    // UI-thread geometry writers (setPageSize/applyResize), which publish them synchronously when they
    // stage; a writer may have done so while the lock was released earlier, so overwriting them here with
    // this apply's (possibly older) _gridMetrics values would lose that newer published geometry.
    {
        auto const l = std::scoped_lock { _reconfigMutex };
        _publishedMetrics.cellSize = _gridMetrics.cellSize;
        _publishedMetrics.baseline = _gridMetrics.baseline;
        _publishedMetrics.underline = _gridMetrics.underline;
        _publishedMetrics.cellMargin = _gridMetrics.cellMargin;
        _publishedFontDescriptions = _fontDescriptions;
        // Keep the lock-free cell-size mirror in lockstep with _publishedMetrics.cellSize so the two
        // never diverge — both are written here, under the lock, from the same source value.
        _publishedCellSize.store(_gridMetrics.cellSize, std::memory_order_release);
    }
}

bool Renderer::render(vtbackend::Terminal& terminal, bool pressure)
{
    try
    {
        return renderImpl(terminal, pressure);
    }
    catch (std::exception const& e)
    {
        errorLog()("Renderer::render: caught exception: {}", e.what());
    }
    catch (...)
    {
        errorLog()("Renderer::render: caught unknown exception.");
    }
    return false;
}

bool Renderer::renderImpl(vtbackend::Terminal& terminal, bool pressure)
{
    // Hold _applyMutex across the whole frame: this both applies any staged reconfiguration and then
    // renders from _gridMetrics / the texture atlas. Holding it for the full duration makes a GUI-thread
    // applyStagedReconfigDuringSetup() (the minimized/occluded path) wait for an in-flight frame to
    // finish reading those, rather than mutating them mid-render. Uncontended in the steady state (the
    // GUI thread only contends it on the rare not-renderable reconfig), so it adds no per-frame cost
    // beyond one uncontended lock.
    auto const applyGuard = std::scoped_lock { _applyMutex };

    // Apply any geometry/font reconfiguration requested by the UI thread before rendering.
    // This is the only point at which _gridMetrics and the texture atlas are mutated after
    // construction, keeping all such mutation on the render thread (see applyPendingReconfig()).
    applyPendingReconfig();

    auto const statusLineHeight = terminal.statusLineHeight();

    // Reconcile the page size with the terminal's *total* page size. Most page-size changes flow
    // through setPageSize()/applyResize() (which publish synchronously), but the terminal can also
    // change its total page size on its own thread without those — notably toggling the indicator
    // status line on/off (setStatusDisplay() -> resizeScreen()), which adds/removes a line.
    //
    // Only reconcile on a terminal-driven *edge*: the terminal total changed since the previous frame.
    // A bare `_gridMetrics.pageSize != totalPageSize` check would also fire mid-resize, when applyResize()
    // (GUI thread) has already published+staged the new page size — so the render thread applied it into
    // _gridMetrics — but terminal.resizeScreen() (the second half of applyResize(), still on the GUI
    // thread) has not run yet. There the terminal total is the *stale* old value, and reconciling toward
    // it would revert the just-published new size for a frame. By keying on the change of
    // terminal.totalPageSize() itself, an in-flight UI resize is invisible (the terminal total has not
    // moved yet), and when resizeScreen() later lands, _gridMetrics already equals it so the edge is a
    // no-op. A status-line toggle, by contrast, moves the terminal total without any renderer staging, so
    // it registers as an edge and is reconciled.
    auto const totalPageSize = terminal.totalPageSize();
    if (_lastObservedTotalPageSize && *_lastObservedTotalPageSize != totalPageSize
        && _gridMetrics.pageSize != totalPageSize)
    {
        _gridMetrics.pageSize = totalPageSize;
        auto const l = std::scoped_lock { _reconfigMutex };
        _publishedMetrics.pageSize = totalPageSize;
    }
    _lastObservedTotalPageSize = totalPageSize;

    executeImageDiscards();

#if !defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE) // {{{
    // Windows 10 (ConPTY) workaround. ConPTY can't handle non-blocking I/O,
    // so we have to explicitly refresh the render buffer
    // from within the render (reader) thread instead ofthe terminal (writer) thread.
    terminal.refreshRenderBuffer();
#endif // }}}

    auto const smoothPixelOffset = static_cast<int>(terminal.smoothScrollPixelOffset());
    auto const statusDisplayAtTop =
        terminal.settings().statusDisplayPosition == vtbackend::StatusDisplayPosition::Top;
    // The partition boundary separates the two regions in the render buffer.
    // Bottom: [main 0..pageSize) [status pageSize..)   → boundary = pageSize
    // Top:    [status 0..statusHeight) [main statusHeight..)  → boundary = statusHeight
    auto const statusLineBoundary = statusDisplayAtTop ? statusLineHeight : terminal.pageSize().lines;
    auto const now = terminal.currentTime();

    auto cursorOpt = optional<vtbackend::RenderCursor> { std::nullopt };

    // One frame, however many passes it takes: the image renderer stamps its LRU against the frame, so
    // opening it per pass would let a later pass evict what an earlier one already scheduled.
    _imageRenderer.beginFrame();

    // Wraps a render pass: begins image/text passes, invokes the content callback, then ends both.
    auto const renderPass = [&](bool withPressure, auto&& content) {
        _imageRenderer.beginPass();
        _textRenderer.beginFrame();
        _textRenderer.setPressure(withPressure);
        content();
        _textRenderer.endFrame();
        _imageRenderer.endPass();
    };

    auto const primaryPressure = pressure && terminal.isPrimaryScreen();

    if (smoothPixelOffset == 0)
    {
        // --- Single-pass rendering: no smooth scroll offset, no scissor needed ---
        setSmoothScrollOffset(0);
        renderPass(primaryPressure, [&] {
            vtbackend::RenderBufferRef const renderBuffer = terminal.renderBuffer();
            cursorOpt = renderBuffer.get().cursor;
            renderCells(std::span(renderBuffer.get().cells));
            renderLines(std::span(renderBuffer.get().lines));
            renderGutter(std::span(renderBuffer.get().gutter));
        });
    }
    else
    {
        // --- Two-pass rendering: main display with scroll offset, then status line without ---

        // Pass 1: Main display content (with smooth scroll offset, scissored).
        setSmoothScrollOffset(smoothPixelOffset);
        vtbackend::RenderBufferRef const renderBuffer = terminal.renderBuffer();
        cursorOpt = renderBuffer.get().cursor;

        auto const cellSplit = findCellPartitionPoint(renderBuffer.get().cells, statusLineBoundary);
        auto const lineSplit = findLinePartitionPoint(renderBuffer.get().lines, statusLineBoundary);

        // When status line is at bottom, the first partition is main display;
        // when at top, the first partition is the status line.
        auto const firstCells = std::span(renderBuffer.get().cells).first(cellSplit);
        auto const secondCells = std::span(renderBuffer.get().cells).subspan(cellSplit);
        auto const firstLines = std::span(renderBuffer.get().lines).first(lineSplit);
        auto const secondLines = std::span(renderBuffer.get().lines).subspan(lineSplit);

        auto const mainCells = statusDisplayAtTop ? secondCells : firstCells;
        auto const statusCells = statusDisplayAtTop ? firstCells : secondCells;
        auto const mainLines = statusDisplayAtTop ? secondLines : firstLines;
        auto const statusLines = statusDisplayAtTop ? firstLines : secondLines;

        renderPass(primaryPressure, [&] {
            renderCells(mainCells, smoothPixelOffset);
            renderLines(mainLines);
            // The gutter belongs to the main display, and scrolls with it.
            renderGutter(std::span(renderBuffer.get().gutter));
        });

        // Scissor clips the main display area so the offset content doesn't bleed into the status line.
        {
            auto const cellHeight = _gridMetrics.cellSize.height.as<int>();
            auto const mainAreaTop =
                _gridMetrics.pageMargin.top + (statusDisplayAtTop ? *statusLineHeight * cellHeight : 0);
            auto const mainAreaHeight = *terminal.pageSize().lines * cellHeight;
            auto const renderSize = _renderTarget->renderSize();
            auto const renderWidth = renderSize.width.as<int>();
            auto const renderHeight = renderSize.height.as<int>();
            auto const scissorY = renderHeight - (mainAreaTop + mainAreaHeight);
            _renderTarget->setScissorRect(0, scissorY, renderWidth, mainAreaHeight);
            auto const scissorGuard = crispy::Finally([this] { _renderTarget->clearScissorRect(); });
            _renderTarget->execute(now);
        }

        // Pass 2: Status line (no scroll offset, no scissor).
        setSmoothScrollOffset(0);
        renderPass(false, [&] {
            renderCells(statusCells);
            renderLines(statusLines);
        });
    }

    if (cursorOpt)
    {
        auto const cursor = *cursorOpt;

        // When smooth-scrolling is active, flush pending status line commands first (unclipped),
        // so the cursor can be flushed separately within a scissor rect.
        if (smoothPixelOffset != 0)
            _renderTarget->execute(now);

        auto const isAnimating = cursor.animateFrom.has_value() && cursor.animationProgress < 1.0f;
        if (isAnimating)
        {
            auto const fromPixel = _gridMetrics.map(*cursor.animateFrom, smoothPixelOffset);
            auto const toPixel = _gridMetrics.map(cursor.position, smoothPixelOffset);
            auto const animationProgress = cursor.animationProgress;
            auto const interpolated = crispy::Point {
                .x = fromPixel.x
                     + static_cast<int>(animationProgress * static_cast<float>(toPixel.x - fromPixel.x)),
                .y = fromPixel.y
                     + static_cast<int>(animationProgress * static_cast<float>(toPixel.y - fromPixel.y)),
            };
            auto const color =
                cursor.animateFromColor
                    ? vtbackend::mixColor(*cursor.animateFromColor, cursor.cursorColor, animationProgress)
                    : cursor.cursorColor;
            _cursorRenderer.setShape(cursor.shape);
            _cursorRenderer.render(interpolated, cursor.width, color);
        }
        else if (cursor.shape != vtbackend::CursorShape::Block)
        {
            _cursorRenderer.setShape(cursor.shape);
            _cursorRenderer.render(
                _gridMetrics.map(cursor.position, smoothPixelOffset), cursor.width, cursor.cursorColor);
        }

        // Scissor-clip cursor to the main display area to prevent overflow into the status line.
        if (smoothPixelOffset != 0)
        {
            auto const cellHeight = _gridMetrics.cellSize.height.as<int>();
            auto const mainAreaTop =
                _gridMetrics.pageMargin.top + (statusDisplayAtTop ? *statusLineHeight * cellHeight : 0);
            auto const mainAreaHeight = *terminal.pageSize().lines * cellHeight;
            auto const renderSize = _renderTarget->renderSize();
            auto const renderWidth = renderSize.width.as<int>();
            auto const renderHeight = renderSize.height.as<int>();
            auto const scissorY = renderHeight - (mainAreaTop + mainAreaHeight);
            _renderTarget->setScissorRect(0, scissorY, renderWidth, mainAreaHeight);
            auto const scissorGuard = crispy::Finally([this] { _renderTarget->clearScissorRect(); });
            _renderTarget->execute(now);
        }
    }

    _renderTarget->execute(now);

    // Consume the "font reconfig applied" signal here, still under _applyMutex (held for the whole frame).
    // applyPendingReconfig() above sets it on the render thread; consuming it under the same lock that the
    // GUI thread's applyStagedReconfigDuringSetup() uses means the one-shot signal is never double-consumed
    // or lost in a race between the two threads. The caller (paint()) drives the GUI-side geometry recompute
    // when this is true.
    return consumeFontReconfigApplied();
}

void Renderer::renderCells(std::span<vtbackend::RenderCell const> cells, int yPixelOffset)
{
    for (auto const& cell: cells)
    {
        try
        {
            _backgroundRenderer.renderCell(cell);
            _decorationRenderer.renderCell(cell);
            _textRenderer.renderCell(cell);
            if (cell.image)
                _imageRenderer.renderImage(_gridMetrics.map(cell.position, yPixelOffset), *cell.image);
        }
        catch (std::exception const& e)
        {
            errorLog()("renderCells: skipping cell at ({},{}) due to exception: {}",
                       cell.position.line,
                       cell.position.column,
                       e.what());
        }
    }
}

void Renderer::setSmoothScrollOffset(int offset)
{
    _backgroundRenderer.setSmoothScrollOffset(offset);
    _decorationRenderer.setSmoothScrollOffset(offset);
    _textRenderer.setSmoothScrollOffset(offset);
}

void Renderer::renderLines(std::span<vtbackend::RenderLine const> lines)
{
    for (auto const& line: lines)
    {
        _backgroundRenderer.renderLine(line);
        _decorationRenderer.renderLine(line);
        _textRenderer.renderLine(line);
    }
}

void Renderer::renderGutter(std::span<vtbackend::RenderGutterCell const> gutter)
{
    for (auto const& marker: gutter)
    {
        // groupStart and groupEnd together make each marker its own shaping group, so it is placed at
        // its own pen position rather than run together with whatever the text renderer saw last.
        auto const cell = vtbackend::RenderCell {
            .codepoints = std::u32string(1, marker.codepoint),
            .image = {},
            .position =
                vtbackend::CellLocation { .line = marker.lineOffset, .column = vtbackend::ColumnOffset(-1) },
            .attributes = marker.attributes,
            .width = 1,
            .sizing = {},
            .groupStart = true,
            .groupEnd = true,
        };

        try
        {
            _backgroundRenderer.renderCell(cell);
            _textRenderer.renderCell(cell);
        }
        catch (std::exception const& e)
        {
            errorLog()(
                "renderGutter: skipping marker on line {} due to exception: {}", marker.lineOffset, e.what());
        }
    }
}

size_t Renderer::findCellPartitionPoint(std::vector<vtbackend::RenderCell> const& cells,
                                        vtbackend::LineCount statusLineBoundary)
{
    auto const boundary = statusLineBoundary.as<vtbackend::LineOffset>();
    auto const it = std::ranges::partition_point(
        cells, [boundary](auto const& cell) { return cell.position.line < boundary; });
    return static_cast<size_t>(std::distance(cells.begin(), it));
}

size_t Renderer::findLinePartitionPoint(std::vector<vtbackend::RenderLine> const& lines,
                                        vtbackend::LineCount statusLineBoundary)
{
    auto const boundary = statusLineBoundary.as<vtbackend::LineOffset>();
    auto const it = std::ranges::partition_point(
        lines, [boundary](auto const& line) { return line.lineOffset < boundary; });
    return static_cast<size_t>(std::distance(lines.begin(), it));
}

void Renderer::inspect(std::ostream& textOutput) const
{
    _textureAtlas->inspect(textOutput);
    for (auto const& renderable: renderables())
        renderable->inspect(textOutput);
}

} // namespace vtrasterizer
