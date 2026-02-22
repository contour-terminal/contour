// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/Renderer.h>
#include <vtrasterizer/TextRenderer.h>
#include <vtrasterizer/utils.h>

#include <text_shaper/font_locator.h>
#include <text_shaper/open_shaper.h>

#include <crispy/StrongLRUHashtable.h>
#include <crispy/utils.h>

#if defined(_WIN32)
    #include <text_shaper/directwrite_shaper.h>
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <span>

using std::array;
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
        gm.cellMargin = {
            .top = 0, .left = 0, .bottom = 0, .right = 0
        }; // TODO (pass as args, and make use of them)
        gm.pageMargin = { .left = 0, .top = 0, .bottom = 0 }; // TODO (fill early)

        loadGridMetricsFromFont(font, gm, textShaper);

        return gm;
    }

    FontKeys loadFontKeys(FontDescriptions const& fd, text::shaper& shaper)
    {
        FontKeys output {};
        auto const regularOpt = shaper.load_font(fd.regular, fd.size);
        if (!SoftRequire(regularOpt.has_value()))
            return output; // Return default-constructed FontKeys if regular font fails to load.
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
    _atlasTileCount {
        std::max(atlasTileCount.value, static_cast<uint32_t>(pageSize.area() * 3))
    }, // TODO instead of pagesize use size for fullscreen window
       // 3 required for huge sixel images rendering due to initial page size smaller than
    _atlasDirectMapping { atlasDirectMapping },
    //.
    _fontDescriptions { std::move(fontDescriptions) },
    _textShaper { [&] {
        auto shaper = createTextShaper(_fontDescriptions.textShapingEngine,
                                       _fontDescriptions.dpi,
                                       createFontLocator(_fontDescriptions.fontLocator));
        shaper->set_font_fallback_limit(_fontDescriptions.maxFallbackCount);
        return shaper;
    }() },
    _fonts { loadFontKeys(_fontDescriptions, *_textShaper) },
    _gridMetrics { loadGridMetrics(_fonts.regular, pageSize, *_textShaper) },
    //.
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

void Renderer::configureTextureAtlas()
{
    Require(_renderTarget);

    auto const atlasCellSize = _gridMetrics.cellSize;
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
    if (fontDescriptions == _fontDescriptions)
        return;

    // When only DPI changed, the enhanced set_dpi() updates existing FT_Face
    // objects in-place. Skip clear_cache() to avoid destroying them.
    auto descriptionsWithSameDpi = fontDescriptions;
    descriptionsWithSameDpi.dpi = _fontDescriptions.dpi;
    auto const onlyDpiChanged = (descriptionsWithSameDpi == _fontDescriptions);

    if (_fontDescriptions.textShapingEngine == fontDescriptions.textShapingEngine)
    {
        if (!onlyDpiChanged)
            _textShaper->clear_cache();
        _textShaper->set_dpi(fontDescriptions.dpi);
        _textShaper->set_font_fallback_limit(fontDescriptions.maxFallbackCount);
        if (_fontDescriptions.fontLocator != fontDescriptions.fontLocator)
            _textShaper->set_locator(createFontLocator(fontDescriptions.fontLocator));
    }
    else
    {
        _textShaper = createTextShaper(fontDescriptions.textShapingEngine,
                                       fontDescriptions.dpi,
                                       createFontLocator(fontDescriptions.fontLocator));
        _textShaper->set_font_fallback_limit(fontDescriptions.maxFallbackCount);
    }

    _fontDescriptions = std::move(fontDescriptions);
    _fonts = loadFontKeys(_fontDescriptions, *_textShaper);
    updateFontMetrics();

    if (_renderTarget)
        _renderTarget->setTextOutline(_fontDescriptions.textOutline.thickness,
                                      _fontDescriptions.textOutline.color);
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
    try
    {
        renderImpl(terminal, pressure);
    }
    catch (std::exception const& e)
    {
        errorLog()("Renderer::render: caught exception: {}", e.what());
    }
    catch (...)
    {
        errorLog()("Renderer::render: caught unknown exception.");
    }
}

void Renderer::renderImpl(vtbackend::Terminal& terminal, bool pressure)
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

    auto const smoothPixelOffset = static_cast<int>(terminal.smoothScrollPixelOffset());
    auto const statusDisplayAtTop =
        terminal.settings().statusDisplayPosition == vtbackend::StatusDisplayPosition::Top;
    // The partition boundary separates the two regions in the render buffer.
    // Bottom: [main 0..pageSize) [status pageSize..)   → boundary = pageSize
    // Top:    [status 0..statusHeight) [main statusHeight..)  → boundary = statusHeight
    auto const statusLineBoundary = statusDisplayAtTop ? statusLineHeight : terminal.pageSize().lines;
    auto const now = terminal.currentTime();

    auto cursorOpt = optional<vtbackend::RenderCursor> { std::nullopt };

    // Wraps a render pass: begins image/text frames, invokes the content callback, then ends both frames.
    auto const renderPass = [&](bool withPressure, auto&& content) {
        _imageRenderer.beginFrame();
        _textRenderer.beginFrame();
        _textRenderer.setPressure(withPressure);
        content();
        _textRenderer.endFrame();
        _imageRenderer.endFrame();
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
            auto const scissorGuard = crispy::finally([this] { _renderTarget->clearScissorRect(); });
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
            auto const interpolated = crispy::point {
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
            auto const scissorGuard = crispy::finally([this] { _renderTarget->clearScissorRect(); });
            _renderTarget->execute(now);
        }
    }

    _renderTarget->execute(now);
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
