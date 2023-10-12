// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/Renderer.h>
#include <vtrasterizer/TextRenderer.h>
#include <vtrasterizer/utils.h>

#include <text_shaper/font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/StrongLRUHashtable.h>

#if defined(_WIN32)
    #include <text_shaper/directwrite_shaper.h>
#endif

#include <array>
#include <memory>

using std::array;
using std::get;
using std::holds_alternative;
using std::initializer_list;
using std::make_unique;
using std::move;
using std::nullopt;
using std::optional;
using std::scoped_lock;
using std::tuple;
using std::unique_ptr;
using std::vector;
using std::chrono::steady_clock;

namespace vtrasterizer
{

namespace
{

    void loadGridMetricsFromFont(text::font_key font, GridMetrics& gm, text::shaper& textShaper)
    {
        auto const m = textShaper.metrics(font);

        gm.cellSize.width = vtbackend::Width::cast_from(m.advance);
        gm.cellSize.height = vtbackend::Height::cast_from(m.lineHeight);
        gm.baseline = m.lineHeight - m.ascender;
        gm.underline.position = gm.baseline + m.underlinePosition;
        gm.underline.thickness = m.underlineThickness;

        rendererLog()("Loading grid metrics {}", gm);
    }

    GridMetrics loadGridMetrics(text::font_key font, vtbackend::PageSize pageSize, text::shaper& textShaper)
    {
        auto gm = GridMetrics {};

        gm.pageSize = pageSize;
        gm.cellMargin = { 0, 0, 0, 0 }; // TODO (pass as args, and make use of them)
        gm.pageMargin = { 0, 0, 0 };    // TODO (fill early)

        loadGridMetricsFromFont(font, gm, textShaper);

        return gm;
    }

    FontKeys loadFontKeys(FontDescriptions const& fd, text::shaper& shaper)
    {
        FontKeys output {};
        auto const regularOpt = shaper.load_font(fd.regular, fd.size);
        Require(regularOpt.has_value());
        output.regular = regularOpt.value();
        output.bold = shaper.load_font(fd.bold, fd.size).value_or(output.regular);
        output.italic = shaper.load_font(fd.italic, fd.size).value_or(output.regular);
        output.boldItalic = shaper.load_font(fd.boldItalic, fd.size).value_or(output.regular);
        output.emoji = shaper.load_font(fd.emoji, fd.size).value_or(output.regular);

        return output;
    }

    unique_ptr<text::shaper> createTextShaper(TextShapingEngine engine, DPI dpi, text::font_locator& locator)
    {
        switch (engine)
        {
            case TextShapingEngine::DWrite:
#if defined(_WIN32)
                rendererLog()("Using DirectWrite text shaping engine.");
                // TODO: do we want to use custom font locator here?
                return make_unique<text::directwrite_shaper>(dpi, locator);
#else
                rendererLog()("DirectWrite not available on this platform.");
                break;
#endif

            case TextShapingEngine::CoreText:
#if defined(__APPLE__)
                rendererLog()("CoreText not yet implemented.");
                break;
#else
                rendererLog()("CoreText not available on this platform.");
                break;
#endif

            case TextShapingEngine::OpenShaper: break;
        }

        rendererLog()("Using OpenShaper text shaping engine.");
        return make_unique<text::open_shaper>(dpi, locator);
    }

} // namespace

Renderer::Renderer(vtbackend::PageSize pageSize,
                   FontDescriptions fontDescriptions,
                   vtbackend::ColorPalette const& colorPalette,
                   crispy::strong_hashtable_size atlasHashtableSlotCount,
                   crispy::lru_capacity atlasTileCount,
                   bool atlasDirectMapping,
                   Decorator hyperlinkNormal,
                   Decorator hyperlinkHover):
    _atlasHashtableSlotCount { crispy::nextPowerOfTwo(atlasHashtableSlotCount.value) },
    _atlasTileCount { std::max(atlasTileCount.value, static_cast<uint32_t>(pageSize.area())) },
    _atlasDirectMapping { atlasDirectMapping },
    //.
    _fontDescriptions { std::move(fontDescriptions) },
    _textShaper { createTextShaper(_fontDescriptions.textShapingEngine,
                                   _fontDescriptions.dpi,
                                   createFontLocator(_fontDescriptions.fontLocator)) },
    _fonts { loadFontKeys(_fontDescriptions, *_textShaper) },
    _gridMetrics { loadGridMetrics(_fonts.regular, pageSize, *_textShaper) },
    //.
    _colorPalette { colorPalette },
    _backgroundRenderer { _gridMetrics, colorPalette.defaultBackground },
    _imageRenderer { _gridMetrics, cellSize() },
    _textRenderer { _gridMetrics, *_textShaper, _fontDescriptions, _fonts, _imageRenderer },
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
    _directMappingAllocator = atlas::DirectMappingAllocator<RenderTileAttributes> { 1 };

    // Explicitly enable direct mapping for everything BUT the text renderer.
    // Only the text renderer's direct mapping is configurable (for simplicity for now).
    _directMappingAllocator.enabled = true;
    for (Renderable* renderable: initializer_list<Renderable*> {
             &_backgroundRenderer, &_cursorRenderer, &_decorationRenderer, &_imageRenderer })
        renderable->setRenderTarget(renderTarget, _directMappingAllocator);
    _directMappingAllocator.enabled = _atlasDirectMapping;
    _textRenderer.setRenderTarget(renderTarget, _directMappingAllocator);

    configureTextureAtlas();
}

void Renderer::configureTextureAtlas()
{
    Require(_renderTarget);

    auto const atlasCellSize = _gridMetrics.cellSize;
    auto atlasProperties = atlas::AtlasProperties { atlas::Format::RGBA,
                                                    atlasCellSize,
                                                    _atlasHashtableSlotCount,
                                                    _atlasTileCount,
                                                    _directMappingAllocator.currentlyAllocatedCount };

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

    for (gsl::not_null<Renderable*>& renderable: renderables())
        renderable->setTextureAtlas(*_textureAtlas);
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
    if (!_renderTarget)
        return;

    _renderTarget->clearCache();

    // TODO(?): below functions are actually doing the same again and again and again. delete them (and their
    // functions for that) either that, or only the render target is allowed to clear the actual atlas caches.
    for (auto& renderable: renderables())
        renderable->clearCache();
}

void Renderer::setFonts(FontDescriptions fontDescriptions)
{
    if (_fontDescriptions.textShapingEngine == fontDescriptions.textShapingEngine)
    {
        _textShaper->clear_cache();
        _textShaper->set_dpi(fontDescriptions.dpi);
        if (_fontDescriptions.fontLocator != fontDescriptions.fontLocator)
            _textShaper->set_locator(createFontLocator(fontDescriptions.fontLocator));
    }
    else
        _textShaper = createTextShaper(fontDescriptions.textShapingEngine,
                                       fontDescriptions.dpi,
                                       createFontLocator(fontDescriptions.fontLocator));

    _fontDescriptions = std::move(fontDescriptions);
    _fonts = loadFontKeys(_fontDescriptions, *_textShaper);
    updateFontMetrics();
}

bool Renderer::setFontSize(text::font_size fontSize)
{
    if (fontSize.pt < 5.) // Let's not be crazy.
        return false;

    if (fontSize.pt > 200.)
        return false;

    _fontDescriptions.size = fontSize;
    _fonts = loadFontKeys(_fontDescriptions, *_textShaper);
    updateFontMetrics();

    return true;
}

void Renderer::updateFontMetrics()
{
    rendererLog()("Updating grid metrics: {}", _gridMetrics);

    _gridMetrics = loadGridMetrics(_fonts.regular, _gridMetrics.pageSize, *_textShaper);

    if (_renderTarget)
        configureTextureAtlas();

    _textRenderer.updateFontMetrics();
    _imageRenderer.setCellSize(cellSize());

    clearCache();
}

void Renderer::render(vtbackend::Terminal& terminal, bool pressure)
{
    auto const statusLineHeight = terminal.statusLineHeight();
    _gridMetrics.pageSize = terminal.pageSize() + statusLineHeight;

    executeImageDiscards();

#if !defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE) // {{{
    // Windows 10 (ConPTY) workaround. ConPTY can't handle non-blocking I/O,
    // so we have to explicitly refresh the render buffer
    // from within the render (reader) thread instead ofthe terminal (writer) thread.
    terminal.refreshRenderBuffer();
#endif // }}}

    optional<vtbackend::RenderCursor> cursorOpt;
    _imageRenderer.beginFrame();
    _textRenderer.beginFrame();
    _textRenderer.setPressure(pressure && terminal.isPrimaryScreen());
    {
        vtbackend::RenderBufferRef const renderBuffer = terminal.renderBuffer();
        cursorOpt = renderBuffer.get().cursor;
        renderCells(renderBuffer.get().cells);
        renderLines(renderBuffer.get().lines);
    }
    _textRenderer.endFrame();
    _imageRenderer.endFrame();

    if (cursorOpt && cursorOpt.value().shape != vtbackend::CursorShape::Block)
    {
        // Note. Block cursor is implicitly rendered via standard grid cell rendering.
        auto const cursor = *cursorOpt;
        _cursorRenderer.setShape(cursor.shape);
        auto const cursorColor = [&]() {
            if (holds_alternative<vtbackend::CellForegroundColor>(_colorPalette.cursor.color))
                return _colorPalette.defaultForeground;
            else if (holds_alternative<vtbackend::CellBackgroundColor>(_colorPalette.cursor.color))
                return _colorPalette.defaultBackground;
            else
                return get<vtbackend::RGBColor>(_colorPalette.cursor.color);
        }();
        _cursorRenderer.render(_gridMetrics.map(cursor.position), cursor.width, cursorColor);
    }

    _renderTarget->execute(terminal.currentTime());
}

void Renderer::renderCells(vector<vtbackend::RenderCell> const& renderableCells)
{
    for (vtbackend::RenderCell const& cell: renderableCells)
    {
        _backgroundRenderer.renderCell(cell);
        _decorationRenderer.renderCell(cell);
        _textRenderer.renderCell(cell);
        if (cell.image)
            _imageRenderer.renderImage(_gridMetrics.map(cell.position), *cell.image);
    }
}

void Renderer::renderLines(vector<vtbackend::RenderLine> const& renderableLines)
{
    for (vtbackend::RenderLine const& line: renderableLines)
    {
        _backgroundRenderer.renderLine(line);
        _decorationRenderer.renderLine(line);
        _textRenderer.renderLine(line);
    }
}

void Renderer::inspect(std::ostream& textOutput) const
{
    _textureAtlas->inspect(textOutput);
    for (auto const& renderable: renderables())
        renderable->inspect(textOutput);
}

} // namespace vtrasterizer
