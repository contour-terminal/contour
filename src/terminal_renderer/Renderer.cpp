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

#include <text_shaper/open_shaper.h>

#if defined(_WIN32)
#include <text_shaper/directwrite_locator.h>
#include <text_shaper/directwrite_shaper.h>
#elif defined(__APPLE__)
#include <text_shaper/coretext_locator.h>
#endif

#include <text_shaper/fontconfig_locator.h>

#include <array>
#include <functional>
#include <memory>

using std::array;
using std::chrono::steady_clock;
using std::get;
using std::holds_alternative;
using std::make_unique;
using std::move;
using std::nullopt;
using std::optional;
using std::reference_wrapper;
using std::scoped_lock;
using std::tuple;
using std::unique_ptr;
using std::vector;

namespace terminal::renderer {

void loadGridMetricsFromFont(text::font_key _font, GridMetrics& _gm, text::shaper& _textShaper)
{
    auto const m = _textShaper.metrics(_font);

    _gm.cellSize.width = Width(m.advance);
    _gm.cellSize.height = Height(m.line_height);
    _gm.baseline = m.line_height - m.ascender;
    _gm.underline.position = _gm.baseline + m.underline_position;
    _gm.underline.thickness = m.underline_thickness;

    LOGSTORE(RasterizerLog)("Loading grid metrics {}", _gm);
}

GridMetrics loadGridMetrics(text::font_key _font, PageSize _pageSize, text::shaper& _textShaper)
{
    auto gm = GridMetrics{};

    gm.pageSize = _pageSize;
    gm.cellMargin = {0, 0, 0, 0}; // TODO (pass as args, and make use of them)
    gm.pageMargin = {0, 0};       // TODO (fill early)

    loadGridMetricsFromFont(_font, gm, _textShaper);

    return gm;
}

FontKeys loadFontKeys(FontDescriptions const& _fd, text::shaper& _shaper)
{
    FontKeys output{};

    output.regular = _shaper.load_font(_fd.regular, _fd.size).value_or(text::font_key{});
    output.bold = _shaper.load_font(_fd.bold, _fd.size).value_or(text::font_key{});
    output.italic = _shaper.load_font(_fd.italic, _fd.size).value_or(text::font_key{});
    output.boldItalic = _shaper.load_font(_fd.boldItalic, _fd.size).value_or(text::font_key{});
    output.emoji = _shaper.load_font(_fd.emoji, _fd.size).value_or(text::font_key{});

    return output;
}

unique_ptr<text::font_locator> createFontLocator(FontLocatorEngine _engine)
{
    switch (_engine)
    {
        // TODO: GDI / DirectWrite needs to be hooked in here
        case FontLocatorEngine::DWrite:
            #if defined(_WIN32)
            return make_unique<text::directwrite_locator>();
            #else
            LOGSTORE(RasterizerLog)("Font locator DirectWrite not supported on this platform.");
            #endif
            break;
        case FontLocatorEngine::CoreText:
            #if defined(__APPLE__)
            return make_unique<text::coretext_locator>();
            #else
            LOGSTORE(RasterizerLog)("Font locator CoreText not supported on this platform.");
            #endif
            break;

        case FontLocatorEngine::FontConfig:
            break;
    }

    LOGSTORE(RasterizerLog)("Using font locator: fontconfig.");
    return make_unique<text::fontconfig_locator>();
}

unique_ptr<text::shaper> createTextShaper(TextShapingEngine _engine, crispy::Point _dpi,
                                          unique_ptr<text::font_locator> _locator)
{
    switch (_engine)
    {
        case TextShapingEngine::DWrite:
            #if defined(_WIN32)
            LOGSTORE(RasterizerLog)("Using DirectWrite text shaping engine.");
            // TODO: do we want to use custom font locator here?
            return make_unique<text::directwrite_shaper>(_dpi, move(_locator));
            #else
            LOGSTORE(RasterizerLog)("DirectWrite not available on this platform.");
            break;
            #endif

        case TextShapingEngine::CoreText:
            #if defined(__APPLE__)
            LOGSTORE(RasterizerLog)("CoreText not yet implemented.");
            break;
            #else
            LOGSTORE(RasterizerLog)("CoreText not available on this platform.");
            break;
            #endif

        case TextShapingEngine::OpenShaper:
            break;
    }

    LOGSTORE(RasterizerLog)("Using OpenShaper text shaping engine.");
    return make_unique<text::open_shaper>(_dpi, std::move(_locator));
}

Renderer::Renderer(PageSize _screenSize,
                   FontDescriptions const& _fontDescriptions,
                   terminal::ColorPalette const& _colorPalette,
                   terminal::Opacity _backgroundOpacity,
                   Decorator _hyperlinkNormal,
                   Decorator _hyperlinkHover):
    textShaper_{
        createTextShaper(
            _fontDescriptions.textShapingEngine,
            _fontDescriptions.dpi,
            createFontLocator(_fontDescriptions.fontLocator)
        )
    },
    fontDescriptions_{ _fontDescriptions },
    fonts_{ loadFontKeys(fontDescriptions_, *textShaper_) },
    gridMetrics_{ loadGridMetrics(fonts_.regular, _screenSize, *textShaper_) },
    colorPalette_{ _colorPalette },
    backgroundOpacity_{ _backgroundOpacity },
    backgroundRenderer_{ gridMetrics_, _colorPalette.defaultBackground },
    imageRenderer_{ cellSize() },
    textRenderer_{ gridMetrics_, *textShaper_, fontDescriptions_, fonts_ },
    decorationRenderer_{ gridMetrics_, _hyperlinkNormal, _hyperlinkHover },
    cursorRenderer_{ gridMetrics_, CursorShape::Block }
{
}

void Renderer::setRenderTarget(RenderTarget& _renderTarget)
{
    renderTarget_ = &_renderTarget;
    Renderable::setRenderTarget(_renderTarget);

    for (reference_wrapper<Renderable>& renderable: renderables())
        renderable.get().setRenderTarget(_renderTarget);
}

void Renderer::discardImage(Image const& _image)
{
    // Defer rendering into the renderer thread & render stage, as this call might have
    // been coming out of bounds from another thread (e.g. the terminal's screen update thread)
    auto _l = scoped_lock{imageDiscardLock_};
    discardImageQueue_.emplace_back(_image.id());
}

void Renderer::executeImageDiscards()
{
    auto _l = scoped_lock{imageDiscardLock_};

    for (auto const imageId: discardImageQueue_)
        imageRenderer_.discardImage(imageId);

    discardImageQueue_.clear();
}

void Renderer::clearCache()
{
    if (!renderTargetAvailable())
        return;

    renderTarget().clearCache();

    // TODO(?): below functions are actually doing the same again and again and again. delete them (and their functions for that)
    // either that, or only the render target is allowed to clear the actual atlas caches.
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
        textShaper_ = createTextShaper(
            _fontDescriptions.textShapingEngine,
            _fontDescriptions.dpi,
            createFontLocator(_fontDescriptions.fontLocator)
        );

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
    gridMetrics_ = loadGridMetrics(fonts_.regular, gridMetrics_.pageSize, *textShaper_);

    textRenderer_.updateFontMetrics();
    imageRenderer_.setCellSize(cellSize());

    clearCache();
}

void Renderer::setRenderSize(ImageSize _size)
{
    if (!renderTargetAvailable())
        return;

    renderTarget().setRenderSize(_size);
}

void Renderer::setBackgroundOpacity(terminal::Opacity _opacity)
{
    backgroundOpacity_ = _opacity;
}

uint64_t Renderer::render(Terminal& _terminal, bool _pressure)
{
    gridMetrics_.pageSize = _terminal.screenSize();

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
    textRenderer_.setPressure(_pressure && _terminal.screen().isPrimaryScreen());
    {
        RenderBufferRef const renderBuffer = _terminal.renderBuffer();
        cursorOpt = renderBuffer.get().cursor;
        renderCells(renderBuffer.get().screen);
    }
    textRenderer_.endFrame();

    if (cursorOpt && _terminal.cursorShape() != CursorShape::Block)
    {
        // Note. Block cursor is implicitly rendered via standard grid cell rendering.
        auto const cursor = *cursorOpt;
        cursorRenderer_.setShape(cursor.shape);
        auto const cursorColor = [&]()
        {
            if (holds_alternative<CellForegroundColor>(colorPalette_.cursor.color))
                return colorPalette_.defaultForeground;
            else if (holds_alternative<CellBackgroundColor>(colorPalette_.cursor.color))
                return colorPalette_.defaultBackground;
            else
                return get<RGBColor>(colorPalette_.cursor.color);
        }();
        cursorRenderer_.render(gridMetrics_.map(cursor.position), cursor.width, cursorColor);
    }

    renderTarget().execute();

    return changes;
}

tuple<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette, Cell const& _cell, bool _reverseVideo, bool _selected)
{
    auto const [fg, bg] = _cell.makeColors(_colorPalette, _reverseVideo);
    if (!_selected)
        return tuple{fg, bg};

    auto const a = _colorPalette.selectionForeground.value_or(bg);
    auto const b = _colorPalette.selectionBackground.value_or(fg);
    return tuple{a, b};
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
    return CellFlags{};
}

void Renderer::renderCells(vector<RenderCell> const& _renderableCells)
{
    for (RenderCell const& cell: _renderableCells)
    {
        backgroundRenderer_.renderCell(cell);
        decorationRenderer_.renderCell(cell);
        textRenderer_.renderCell(cell);
        if (cell.image.has_value())
            imageRenderer_.renderImage(gridMetrics_.map(cell.position), *cell.image);
    }
}

optional<RenderCursor> Renderer::renderCursor(Terminal const& _terminal)
{
    bool const shouldDisplayCursor = _terminal.screen().cursor().visible
        && (_terminal.cursorDisplay() == CursorDisplay::Steady || _terminal.cursorBlinkActive());

    if (!shouldDisplayCursor || !_terminal.viewport().isLineVisible(_terminal.screen().cursor().position.line))
        return nullopt;

    // TODO: check if CursorStyle has changed, and update render context accordingly.

    Cell const& cursorCell = _terminal.screen().at(_terminal.screen().cursor().position);

    auto const cursorShape = _terminal.screen().focused() ? _terminal.cursorShape()
                                                          : CursorShape::Rectangle;

    return RenderCursor{
        gridMetrics_.map(
            _terminal.screen().cursor().position.line
                + _terminal.viewport().scrollOffset().as<LineOffset>(),
            _terminal.screen().cursor().position.column
        ),
        cursorShape,
        cursorCell.width()
    };
}

void Renderer::dumpState(std::ostream& _textOutput) const
{
    textRenderer_.debugCache(_textOutput);
}

} // end namespace
