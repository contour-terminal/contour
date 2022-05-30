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
#include <terminal_renderer/Renderer.h>
#include <terminal_renderer/TextRenderer.h>
#include <terminal_renderer/utils.h>

#include <text_shaper/font_locator.h>
#include <text_shaper/open_shaper.h>

#if defined(_WIN32)
    #include <text_shaper/directwrite_shaper.h>
#endif

#include <array>
#include <functional>
#include <memory>

using std::array;
using std::get;
using std::holds_alternative;
using std::initializer_list;
using std::make_unique;
using std::move;
using std::nullopt;
using std::optional;
using std::reference_wrapper;
using std::scoped_lock;
using std::tuple;
using std::unique_ptr;
using std::vector;
using std::chrono::steady_clock;

namespace terminal::renderer
{

void loadGridMetricsFromFont(text::font_key _font, GridMetrics& _gm, text::shaper& _textShaper)
{
    auto const m = _textShaper.metrics(_font);

    _gm.cellSize.width = Width::cast_from(m.advance);
    _gm.cellSize.height = Height::cast_from(m.line_height);
    _gm.baseline = m.line_height - m.ascender;
    _gm.underline.position = _gm.baseline + m.underline_position;
    _gm.underline.thickness = m.underline_thickness;

    RendererLog()("Loading grid metrics {}", _gm);
}

GridMetrics loadGridMetrics(text::font_key _font, PageSize _pageSize, text::shaper& _textShaper)
{
    auto gm = GridMetrics {};

    gm.pageSize = _pageSize;
    gm.cellMargin = { 0, 0, 0, 0 }; // TODO (pass as args, and make use of them)
    gm.pageMargin = { 0, 0 };       // TODO (fill early)

    loadGridMetricsFromFont(_font, gm, _textShaper);

    return gm;
}

FontKeys loadFontKeys(FontDescriptions const& _fd, text::shaper& _shaper)
{
    FontKeys output {};
    auto const regularOpt = _shaper.load_font(_fd.regular, _fd.size);
    Require(regularOpt.has_value());
    output.regular = regularOpt.value();
    output.bold = _shaper.load_font(_fd.bold, _fd.size).value_or(output.regular);
    output.italic = _shaper.load_font(_fd.italic, _fd.size).value_or(output.regular);
    output.boldItalic = _shaper.load_font(_fd.boldItalic, _fd.size).value_or(output.regular);
    output.emoji = _shaper.load_font(_fd.emoji, _fd.size).value_or(output.regular);

    return output;
}

unique_ptr<text::shaper> createTextShaper(TextShapingEngine _engine,
                                          DPI _dpi,
                                          unique_ptr<text::font_locator> _locator)
{
    switch (_engine)
    {
        case TextShapingEngine::DWrite:
#if defined(_WIN32)
            RendererLog()("Using DirectWrite text shaping engine.");
            // TODO: do we want to use custom font locator here?
            return make_unique<text::directwrite_shaper>(_dpi, move(_locator));
#else
            RendererLog()("DirectWrite not available on this platform.");
            break;
#endif

        case TextShapingEngine::CoreText:
#if defined(__APPLE__)
            RendererLog()("CoreText not yet implemented.");
            break;
#else
            RendererLog()("CoreText not available on this platform.");
            break;
#endif

        case TextShapingEngine::OpenShaper: break;
    }

    RendererLog()("Using OpenShaper text shaping engine.");
    return make_unique<text::open_shaper>(_dpi, std::move(_locator));
}

Renderer::Renderer(PageSize pageSize,
                   FontDescriptions fontDescriptions,
                   terminal::ColorPalette const& colorPalette,
                   terminal::Opacity backgroundOpacity,
                   crispy::StrongHashtableSize atlasHashtableSlotCount,
                   crispy::LRUCapacity atlasTileCount,
                   bool atlasDirectMapping,
                   Decorator hyperlinkNormal,
                   Decorator hyperlinkHover):
    _atlasHashtableSlotCount { crispy::nextPowerOfTwo(atlasHashtableSlotCount.value) },
    _atlasTileCount { std::max(atlasTileCount.value, static_cast<uint32_t>(pageSize.area())) },
    _atlasDirectMapping { atlasDirectMapping },
    _renderTarget { nullptr },
    //.
    fontDescriptions_ { move(fontDescriptions) },
    textShaper_ { createTextShaper(fontDescriptions_.textShapingEngine,
                                   fontDescriptions_.dpi,
                                   createFontLocator(fontDescriptions_.fontLocator)) },
    fonts_ { loadFontKeys(fontDescriptions_, *textShaper_) },
    gridMetrics_ { loadGridMetrics(fonts_.regular, pageSize, *textShaper_) },
    //.
    colorPalette_ { colorPalette },
    backgroundOpacity_ { backgroundOpacity },
    backgroundRenderer_ { gridMetrics_, colorPalette.defaultBackground },
    imageRenderer_ { gridMetrics_, cellSize() },
    textRenderer_ { gridMetrics_, *textShaper_, fontDescriptions_, fonts_ },
    decorationRenderer_ { gridMetrics_, hyperlinkNormal, hyperlinkHover },
    cursorRenderer_ { gridMetrics_, CursorShape::Block }
{
    textRenderer_.updateFontMetrics();
    imageRenderer_.setCellSize(cellSize());

    // clang-format off
    if (_atlasTileCount.value > atlasTileCount.value)
        RendererLog()("Increasing atlas tile count configuration to {} to satisfy worst-case rendering scenario.",
                              _atlasTileCount.value);

    if (_atlasHashtableSlotCount.value > atlasHashtableSlotCount.value)
        RendererLog()("Increasing atlas hashtable slot count configuration to the next power of two: {}.",
                              _atlasHashtableSlotCount.value);
    // clang-format on
}

void Renderer::setRenderTarget(RenderTarget& renderTarget)
{
    _renderTarget = &renderTarget;

    // Reset DirectMappingAllocator (also skipping zero-tile).
    directMappingAllocator_ = atlas::DirectMappingAllocator<RenderTileAttributes> { 1 };

    // Explicitly enable direct mapping for everything BUT the text renderer.
    // Only the text renderer's direct mapping is configurable (for simplicity for now).
    directMappingAllocator_.enabled = true;
    for (Renderable* renderable: initializer_list<Renderable*> {
             &backgroundRenderer_, &cursorRenderer_, &decorationRenderer_, &imageRenderer_ })
        renderable->setRenderTarget(renderTarget, directMappingAllocator_);
    directMappingAllocator_.enabled = _atlasDirectMapping;
    textRenderer_.setRenderTarget(renderTarget, directMappingAllocator_);

    configureTextureAtlas();

    if (colorPalette_.backgroundImage)
        renderTarget.setBackgroundImage(colorPalette_.backgroundImage);
}

void Renderer::configureTextureAtlas()
{
    Require(_renderTarget);

    auto atlasProperties =
        atlas::AtlasProperties { atlas::Format::RGBA,
                                 gridMetrics_.cellSize, // Cell size is used as GPU tile size.
                                 _atlasHashtableSlotCount,
                                 _atlasTileCount,
                                 directMappingAllocator_.currentlyAllocatedCount };

    Require(atlasProperties.tileCount.value > 0);

    textureAtlas_ = make_unique<Renderable::TextureAtlas>(_renderTarget->textureScheduler(), atlasProperties);

    // clang-format off
    RendererLog()("Configuring texture atlas.\n", atlasProperties);
    RendererLog()("- Atlas properties     : {}\n", atlasProperties);
    RendererLog()("- Atlas texture size   : {} pixels\n", textureAtlas_->atlasSize());
    RendererLog()("- Atlas hashtable      : {} slots\n", _atlasHashtableSlotCount.value);
    RendererLog()("- Atlas tile count     : {} = {}x * {}y\n", textureAtlas_->capacity(), textureAtlas_->tilesInX(), textureAtlas_->tilesInY());
    RendererLog()("- Atlas direct mapping : {} (for text rendering)", _atlasDirectMapping ? "enabled" : "disabled");
    // clang-format on

    for (reference_wrapper<Renderable>& renderable: renderables())
        renderable.get().setTextureAtlas(*textureAtlas_);
}

void Renderer::discardImage(Image const& _image)
{
    // Defer rendering into the renderer thread & render stage, as this call might have
    // been coming out of bounds from another thread (e.g. the terminal's screen update thread)
    auto _l = scoped_lock { imageDiscardLock_ };
    discardImageQueue_.emplace_back(_image.id());
}

void Renderer::executeImageDiscards()
{
    auto _l = scoped_lock { imageDiscardLock_ };

    for (auto const imageId: discardImageQueue_)
        imageRenderer_.discardImage(imageId);

    discardImageQueue_.clear();
}

void Renderer::clearCache()
{
    if (!_renderTarget)
        return;

    _renderTarget->clearCache();

    // TODO(?): below functions are actually doing the same again and again and again. delete them (and their
    // functions for that) either that, or only the render target is allowed to clear the actual atlas caches.
    for (auto& renderable: renderables())
        renderable.get().clearCache();
}

void Renderer::setFonts(FontDescriptions _fontDescriptions)
{
    if (fontDescriptions_.textShapingEngine == _fontDescriptions.textShapingEngine)
    {
        textShaper_->clear_cache();
        textShaper_->set_dpi(_fontDescriptions.dpi);
        if (fontDescriptions_.fontLocator != _fontDescriptions.fontLocator)
            textShaper_->set_locator(createFontLocator(_fontDescriptions.fontLocator));
    }
    else
        textShaper_ = createTextShaper(_fontDescriptions.textShapingEngine,
                                       _fontDescriptions.dpi,
                                       createFontLocator(_fontDescriptions.fontLocator));

    fontDescriptions_ = move(_fontDescriptions);
    fonts_ = loadFontKeys(fontDescriptions_, *textShaper_);
    updateFontMetrics();
}

bool Renderer::setFontSize(text::font_size _fontSize)
{
    if (_fontSize.pt < 5.) // Let's not be crazy.
        return false;

    if (_fontSize.pt > 200.)
        return false;

    fontDescriptions_.size = _fontSize;
    fonts_ = loadFontKeys(fontDescriptions_, *textShaper_);
    updateFontMetrics();

    return true;
}

void Renderer::updateFontMetrics()
{
    RendererLog()("Updating grid metrics: {}", gridMetrics_);

    gridMetrics_ = loadGridMetrics(fonts_.regular, gridMetrics_.pageSize, *textShaper_);

    if (_renderTarget)
        configureTextureAtlas();

    textRenderer_.updateFontMetrics();
    imageRenderer_.setCellSize(cellSize());

    clearCache();
}

uint64_t Renderer::render(Terminal& _terminal, bool _pressure)
{
    gridMetrics_.pageSize = _terminal.pageSize();

    auto const changes = _terminal.tick(steady_clock::now());

    executeImageDiscards();

#if !defined(LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE) // {{{
    // Windows 10 (ConPTY) workaround. ConPTY can't handle non-blocking I/O,
    // so we have to explicitly refresh the render buffer
    // from within the render (reader) thread instead ofthe terminal (writer) thread.
    _terminal.refreshRenderBuffer();
#endif // }}}

    optional<terminal::RenderCursor> cursorOpt;
    textRenderer_.beginFrame();
    textRenderer_.setPressure(_pressure && _terminal.isPrimaryScreen());
    {
        RenderBufferRef const renderBuffer = _terminal.renderBuffer();
        cursorOpt = renderBuffer.get().cursor;
        renderCells(renderBuffer.get().cells);
    }
    textRenderer_.endFrame();

    if (cursorOpt && cursorOpt.value().shape != CursorShape::Block)
    {
        // Note. Block cursor is implicitly rendered via standard grid cell rendering.
        auto const cursor = *cursorOpt;
        cursorRenderer_.setShape(cursor.shape);
        auto const cursorColor = [&]() {
            if (holds_alternative<CellForegroundColor>(colorPalette_.cursor.color))
                return colorPalette_.defaultForeground;
            else if (holds_alternative<CellBackgroundColor>(colorPalette_.cursor.color))
                return colorPalette_.defaultBackground;
            else
                return get<RGBColor>(colorPalette_.cursor.color);
        }();
        cursorRenderer_.render(gridMetrics_.map(cursor.position), cursor.width, cursorColor);
    }

    _renderTarget->execute();

    return changes;
}

tuple<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette,
                                     Cell const& _cell,
                                     bool _reverseVideo,
                                     bool _selected)
{
    auto const [fg, bg] = _cell.makeColors(_colorPalette, _reverseVideo);
    if (!_selected)
        return tuple { fg, bg };

    auto const a = _colorPalette.selectionForeground.value_or(bg);
    auto const b = _colorPalette.selectionBackground.value_or(fg);
    return tuple { a, b };
}

constexpr CellFlags toCellStyle(Decorator _decorator)
{
    switch (_decorator)
    {
        case Decorator::Underline: return CellFlags::Underline;
        case Decorator::DoubleUnderline: return CellFlags::DoublyUnderlined;
        case Decorator::CurlyUnderline: return CellFlags::CurlyUnderlined;
        case Decorator::DottedUnderline: return CellFlags::DottedUnderline;
        case Decorator::DashedUnderline: return CellFlags::DashedUnderline;
        case Decorator::Overline: return CellFlags::Overline;
        case Decorator::CrossedOut: return CellFlags::CrossedOut;
        case Decorator::Framed: return CellFlags::Framed;
        case Decorator::Encircle: return CellFlags::Encircled;
    }
    return CellFlags {};
}

void Renderer::renderCells(vector<RenderCell> const& _renderableCells)
{
    for (RenderCell const& cell: _renderableCells)
    {
        backgroundRenderer_.renderCell(cell);
        decorationRenderer_.renderCell(cell);
        textRenderer_.renderCell(cell);
        if (cell.image)
            imageRenderer_.renderImage(gridMetrics_.map(cell.position), *cell.image);
    }
}

void Renderer::inspect(std::ostream& _textOutput) const
{
    textureAtlas_->inspect(_textOutput);
    for (auto const& renderable: renderables())
        renderable.get().inspect(_textOutput);
}

} // namespace terminal::renderer
