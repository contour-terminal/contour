// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/ColorPalette.h>
#include <vtbackend/Image.h>
#include <vtbackend/Terminal.h>

#include <vtrasterizer/BackgroundRenderer.h>
#include <vtrasterizer/CursorRenderer.h>
#include <vtrasterizer/DecorationRenderer.h>
#include <vtrasterizer/Decorator.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/ImageRenderer.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextRenderer.h>

#include <crispy/StrongLRUHashtable.h>
#include <crispy/size.h>

#include <gsl/pointers>

#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace vtrasterizer
{

struct RenderCursor
{
    crispy::point position;
    vtbackend::CursorShape shape;
    int width;

    RenderCursor(crispy::point position, vtbackend::CursorShape shape, int width):
        position(position), shape(shape), width(width)
    {
    }
};

/**
 * Renders a terminal's screen to the current OpenGL context.
 */
class Renderer
{
  public:
    friend class RendererTest; //!< Grants unit tests access to the deferred-reconfiguration internals.

    /** Constructs a Renderer instances.
     *
     * @p fonts              Reference to the set of loaded fonts to be used for rendering text.
     * @p colorPalette       User-configurable color profile to use to map terminal colors to.
     * @p projectionMatrix   Projection matrix to apply to the rendered scene when rendering the screen.
     * @p atlasDirectMapping Indicates whether or not direct mapped tiles are allowed.
     * @p atlasTileCount     Number of tiles guaranteed to be available in LRU cache.
     */
    Renderer(vtbackend::PageSize pageSize,
             FontDescriptions fontDescriptions,
             vtbackend::ColorPalette const& colorPalette,
             crispy::strong_hashtable_size atlasHashtableSlotCount,
             crispy::lru_capacity atlasTileCount,
             bool atlasDirectMapping,
             Decorator hyperlinkNormal,
             Decorator hyperlinkHover);

    /// Returns the live cell size from the grid metrics.
    ///
    /// @warning Reads the live _gridMetrics without synchronization; intended for render-thread /
    ///          construction-time use only. UI-thread callers must use gridMetrics().cellSize, which
    ///          returns the published (mutex-guarded) copy.
    [[nodiscard]] ImageSize cellSize() const noexcept { return _gridMetrics.cellSize; }

    /// Initializes the render and all render subsystems with the given RenderTarget
    /// and then informs all renderables about the newly created texture atlas.
    void setRenderTarget(RenderTarget& renderTarget);
    RenderTarget& renderTarget() noexcept { return *_renderTarget; }
    [[nodiscard]] bool hasRenderTarget() const noexcept { return _renderTarget != nullptr; }

    bool setFontSize(text::font_size fontSize);
    void updateFontMetrics();

    /// Returns the most recently *published* font descriptions.
    ///
    /// Returns a snapshot by value (not a reference into the live state): the live _fontDescriptions is
    /// mutated on the render thread during applyPendingReconfig(), so a UI-thread reader must observe a
    /// mutex-guarded copy rather than reading the live field lock-free (which would be a data race).
    /// A font change (setFonts/setFontSize) becomes visible here only after the render thread applies it.
    ///
    /// @note Thread-safe with respect to concurrent reconfiguration requests.
    [[nodiscard]] FontDescriptions fontDescriptions() const noexcept
    {
        auto const l = std::scoped_lock { _reconfigMutex };
        return _publishedFontDescriptions;
    }

    /// Stages a full font-descriptions change to be applied on the render thread.
    ///
    /// The text shaper is not thread-safe and is read by the render thread every frame, so this only
    /// *stages* the descriptions (on the UI thread); the shaper reconfiguration, font loading and
    /// grid-metrics/atlas rebuild happen on the render thread in applyPendingReconfig(). The new cell
    /// size therefore appears via gridMetrics()/fontDescriptions() only after a frame (or a setup-time
    /// applyStagedReconfigDuringSetup()), not synchronously on return. A staged size-only change is
    /// superseded by the full descriptions staged here.
    ///
    /// @param fontDescriptions  the new font configuration to stage.
    void setFonts(FontDescriptions fontDescriptions);

    /// Stages a DPI-only font change without clobbering an already-staged font-descriptions change.
    ///
    /// Unlike reading fontDescriptions() and re-staging the whole thing via setFonts(), this updates
    /// only the .dpi field of the *effective* descriptions — the ones already staged in a pending
    /// reconfig if present, otherwise the live ones. A concurrent font-family change staged just before
    /// (e.g. by a config reload) therefore keeps its family and merely picks up the new DPI, instead of
    /// being silently overwritten with the live descriptions + new DPI.
    ///
    /// @param dpi  the new device pixels-per-inch to apply to the font configuration.
    void setFontDPI(DPI dpi);

    /// Returns the most recently *published* grid metrics.
    ///
    /// Geometry writers (setPageSize/applyResize) publish synchronously; font writers (setFonts/
    /// setFontSize) only stage, so the new cell size appears only after a frame (or setup-time apply).
    ///
    /// @note Thread-safe with respect to concurrent reconfiguration requests.
    [[nodiscard]] GridMetrics gridMetrics() const noexcept
    {
        auto const l = std::scoped_lock { _reconfigMutex };
        return _publishedMetrics;
    }

    /// Returns the most recently *published* cell size, lock-free.
    ///
    /// Equivalent to gridMetrics().cellSize but without taking _reconfigMutex or copying the whole
    /// GridMetrics struct — for the many UI-thread hot paths (per-frame sync, scroll, IME, size
    /// recompute) that need only the cell size. Reading it also cannot tear against a concurrent
    /// render-thread font apply, unlike two separate gridMetrics().cellSize reads.
    [[nodiscard]] ImageSize publishedCellSize() const noexcept
    {
        return _publishedCellSize.load(std::memory_order_acquire);
    }

    /// Atomically reads and clears the "a font reconfiguration was applied" flag.
    ///
    /// A font change (setFontSize/setFonts) is applied lazily on the render thread, so the new cell
    /// size only becomes available after a frame. The display polls this after rendering and, when it
    /// returns true, re-derives the terminal page size against the now-current cell size — otherwise a
    /// font/DPI change would size the grid using the previous cell size for a frame.
    ///
    /// @return true if a font reconfiguration has been applied since the last call.
    [[nodiscard]] bool consumeFontReconfigApplied() noexcept
    {
        return _fontReconfigApplied.exchange(false, std::memory_order_acq_rel);
    }

    void setHyperlinkDecoration(Decorator normal, Decorator hover)
    {
        _decorationRenderer.setHyperlinkDecoration(normal, hover);
    }

    /// Requests a new page size (in columns/lines).
    ///
    /// The change is published immediately (visible via gridMetrics()) and the actual
    /// grid-metrics mutation is applied on the render thread at the start of the next frame.
    ///
    /// @param screenSize  the new page size.
    void setPageSize(vtbackend::PageSize screenSize) noexcept
    {
        auto const l = std::scoped_lock { _reconfigMutex };
        _publishedMetrics.pageSize = screenSize;
        stagePendingGeometryLocked();
    }

    /// Requests a combined render-surface resize: pixel size, page size and margin.
    ///
    /// Consolidates the three geometry writers so the render target and grid metrics are
    /// updated atomically (on the render thread) rather than in three separately-observable steps.
    ///
    /// @param newPixelSize  new render surface size in pixels.
    /// @param newPageSize   new page size in columns/lines.
    /// @param newMargin     new page margin in pixels.
    void applyResize(vtbackend::ImageSize newPixelSize,
                     vtbackend::PageSize newPageSize,
                     PageMargin newMargin) noexcept
    {
        auto const l = std::scoped_lock { _reconfigMutex };
        _publishedMetrics.pageSize = newPageSize;
        _publishedMetrics.pageMargin = newMargin;
        auto& pending = ensurePendingLocked();
        pending.geometry = PendingReconfig::Geometry { .pixelSize = newPixelSize,
                                                       .pageSize = newPageSize,
                                                       .pageMargin = newMargin };
    }

    /**
     * Renders the given @p terminal to the current OpenGL context.
     *
     * @param terminal       The terminal to render
     * @param pressureHint   Indicates whether or not this render will most likely be
     *                       updated right after again, allowing a few optimizations
     *                       to performa that reduce visual features as they are
     *                       CPU intensive but allow to render fast.
     *                       The user shall not notice that, because this frame
     *                       is known already to be updated right after again.
     */
    void render(vtbackend::Terminal& terminal, bool pressureHint);

    /// Synchronously applies any staged font/geometry reconfiguration.
    ///
    /// For display setup, before any frame, when a caller must read the resulting cell metrics
    /// immediately (e.g. to size the texture atlas tile) rather than wait for the render thread.
    ///
    /// @warning Only safe to call while no render thread is concurrently rendering (i.e. during setup).
    void applyStagedReconfigDuringSetup() { applyPendingReconfig(); }

    void discardImage(vtbackend::Image const& image);

    void clearCache();

    void inspect(std::ostream& textOutput) const;

    std::array<gsl::not_null<Renderable*>, 5> renderables()
    {
        return std::array<gsl::not_null<Renderable*>, 5> {
            &_backgroundRenderer, &_cursorRenderer, &_decorationRenderer, &_imageRenderer, &_textRenderer
        };
    }

    [[nodiscard]] std::array<gsl::not_null<Renderable const*>, 5> renderables() const
    {
        return std::array<gsl::not_null<Renderable const*>, 5> {
            &_backgroundRenderer, &_cursorRenderer, &_decorationRenderer, &_imageRenderer, &_textRenderer
        };
    }

    /// Returns the index of the first cell whose line offset >= @p statusLineBoundary.
    ///
    /// @param cells                Sorted vector of render cells ordered by line offset.
    /// @param statusLineBoundary   Line count that separates main display from the status line.
    /// @return Index into @p cells at the partition point; equals cells.size() if all cells
    ///         belong to the main display.
    [[nodiscard]] static size_t findCellPartitionPoint(std::vector<vtbackend::RenderCell> const& cells,
                                                       vtbackend::LineCount statusLineBoundary);

    /// Returns the index of the first line whose offset >= @p statusLineBoundary.
    ///
    /// @param lines                Sorted vector of render lines ordered by line offset.
    /// @param statusLineBoundary   Line count that separates main display from the status line.
    /// @return Index into @p lines at the partition point; equals lines.size() if all lines
    ///         belong to the main display.
    [[nodiscard]] static size_t findLinePartitionPoint(std::vector<vtbackend::RenderLine> const& lines,
                                                       vtbackend::LineCount statusLineBoundary);

  private:
    /// Internal implementation of render(), wrapped in a try/catch for graceful degradation.
    void renderImpl(vtbackend::Terminal& terminal, bool pressure);

    void configureTextureAtlas();

    /// Sets the smooth scroll Y pixel offset on all sub-renderers.
    ///
    /// @param offset  Y pixel offset for smooth scrolling.
    void setSmoothScrollOffset(int offset);

    /// Renders a span of cells to the background, decoration, text, and image renderers.
    ///
    /// @param cells          Contiguous sub-range of RenderCell entries to render.
    /// @param yPixelOffset   Sub-cell Y pixel offset for smooth scrolling (default: 0).
    void renderCells(std::span<vtbackend::RenderCell const> cells, int yPixelOffset = 0);

    /// Renders a span of lines to the background, decoration, and text renderers.
    ///
    /// @param lines  Contiguous sub-range of RenderLine entries to render.
    void renderLines(std::span<vtbackend::RenderLine const> lines);

    void executeImageDiscards();

    crispy::strong_hashtable_size _atlasHashtableSlotCount;
    crispy::lru_capacity _atlasTileCount;
    bool _atlasDirectMapping;

    RenderTarget* _renderTarget = nullptr;

    Renderable::DirectMappingAllocator _directMappingAllocator;
    std::unique_ptr<Renderable::TextureAtlas> _textureAtlas;

    FontDescriptions _fontDescriptions;
    std::unique_ptr<text::shaper> _textShaper;
    FontKeys _fonts;

    /// The live grid metrics, mutated *only* on the render thread (at the start of renderImpl()).
    ///
    /// All sub-renderers hold a const reference to this object and read it throughout a frame,
    /// hence it must never be written while a frame is in flight. UI-thread writers therefore do
    /// not touch it directly; they stage a request (see _pendingReconfig) that the render thread
    /// applies between frames.
    GridMetrics _gridMetrics;

    /// A staged, not-yet-applied reconfiguration request produced by UI-thread writers.
    ///
    /// Carries the *inputs* of a pending change. The render thread consumes it at the top of
    /// renderImpl() to rebuild grid metrics / the texture atlas on the render thread, eliminating
    /// the data race between UI-thread geometry/font changes and the render thread.
    struct PendingReconfig
    {
        /// Pending geometry change: new page size, margin and (optionally) render-surface pixel size.
        struct Geometry
        {
            std::optional<vtbackend::ImageSize> pixelSize; //!< New render surface size, if it changed.
            vtbackend::PageSize pageSize;                  //!< New page size in columns/lines.
            PageMargin pageMargin;                         //!< New page margin in pixels.
        };

        /// Pending font change. Either a size-only change (fontSize) or a full font-descriptions
        /// change (fontDescriptions, which supersedes a size-only change). Only the *inputs* are
        /// staged; the heavyweight work (font loading, metric computation via the non-thread-safe
        /// text shaper, atlas rebuild) is performed on the render thread during applyPendingReconfig().
        std::optional<text::font_size> fontSize;          //!< Set when only the font size changed.
        std::optional<FontDescriptions> fontDescriptions; //!< Set when the full font config changed.
        std::optional<Geometry> geometry;                 //!< Set when a geometry change is pending.
    };

    /// Protects _pendingReconfig and _publishedMetrics, and serializes the render-thread apply.
    mutable std::mutex _reconfigMutex;

    /// The most recently *requested* grid metrics, observable synchronously via gridMetrics().
    ///
    /// Kept in sync by UI-thread writers so callers needing the new cell size immediately are not
    /// blocked on the deferred render-thread apply. Guarded by _reconfigMutex.
    GridMetrics _publishedMetrics;

    /// Mutex-guarded snapshot of _fontDescriptions for UI-thread readers (fontDescriptions(),
    /// setFontDPI()). The live _fontDescriptions is render-thread-owned and mutated without the mutex
    /// during applyPendingReconfig(); a UI-thread reader must therefore observe this guarded copy
    /// instead of racing that write. Published by the render thread (under _reconfigMutex) whenever it
    /// applies a font change, and seeded at construction. Guarded by _reconfigMutex.
    FontDescriptions _publishedFontDescriptions;

    /// Lock-free mirror of _publishedMetrics.cellSize for publishedCellSize(). Written wherever the
    /// published cell size changes (construction and the render-thread font apply); read without the
    /// mutex by UI-thread hot paths. The cell size only changes via font/DPI reconfiguration, never via
    /// geometry writers, so this stays consistent with _publishedMetrics.cellSize.
    std::atomic<ImageSize> _publishedCellSize;

    /// The staged reconfiguration awaiting application by the render thread. Guarded by _reconfigMutex.
    std::optional<PendingReconfig> _pendingReconfig;

    /// Mirrors `_pendingReconfig.has_value()` for a lock-free fast path: applyPendingReconfig() runs on
    /// every frame and consults this first, only acquiring _reconfigMutex when something is actually
    /// staged. Written under _reconfigMutex; read relaxed on the render thread.
    std::atomic<bool> _reconfigPending { false };

    /// Set by applyPendingReconfig() (render thread) when a font change was applied; consumed by the
    /// display (consumeFontReconfigApplied()) to trigger a page-size recompute against the new cell
    /// size. Atomic because it is written on the render thread and read on the display thread.
    std::atomic<bool> _fontReconfigApplied { false };

    /// Ensures a PendingReconfig exists and returns it. Caller must hold _reconfigMutex.
    PendingReconfig& ensurePendingLocked()
    {
        if (!_pendingReconfig)
            _pendingReconfig.emplace();
        _reconfigPending.store(true, std::memory_order_relaxed);
        return *_pendingReconfig;
    }

    /// Stages a geometry-only pending request derived from the current _publishedMetrics.
    ///
    /// A render-surface pixel size already staged by a prior applyResize() is preserved; a page-size-
    /// or margin-only change leaves the pixel size untouched (std::nullopt → render size unchanged).
    /// Caller must hold _reconfigMutex.
    void stagePendingGeometryLocked()
    {
        auto& pending = ensurePendingLocked();
        auto const pixelSize = pending.geometry ? pending.geometry->pixelSize : std::nullopt;
        pending.geometry = PendingReconfig::Geometry { .pixelSize = pixelSize,
                                                       .pageSize = _publishedMetrics.pageSize,
                                                       .pageMargin = _publishedMetrics.pageMargin };
    }

    /// Applies any staged reconfiguration. Called on the render thread at the start of renderImpl().
    void applyPendingReconfig();

    /// Publishes the render-thread-owned font-derived metrics and font descriptions for UI-thread
    /// readers (gridMetrics()/publishedCellSize()/fontDescriptions()).
    ///
    /// Acquires _reconfigMutex for the short copy. Called from applyPendingReconfig() after a font
    /// apply (whether it succeeded or threw), so readers always observe metrics consistent with the
    /// live grid metrics. Does not touch page geometry, which the UI-thread geometry writers own.
    void publishFontMetricsAndDescriptions();

    /// Applies a full font-descriptions change (shaper reconfiguration, font loading, grid-metrics
    /// rebuild, atlas reconfiguration). Runs on the render thread from applyPendingReconfig().
    ///
    /// @param fontDescriptions  the new font descriptions to apply.
    void applyFontDescriptions(FontDescriptions fontDescriptions);

    std::mutex _imageDiscardLock;                       //!< Lock guard for accessing _discardImageQueue.
    std::vector<vtbackend::ImageId> _discardImageQueue; //!< List of images to be discarded.

    BackgroundRenderer _backgroundRenderer;
    ImageRenderer _imageRenderer;
    TextRenderer _textRenderer;
    DecorationRenderer _decorationRenderer;
    CursorRenderer _cursorRenderer;
};

} // namespace vtrasterizer
