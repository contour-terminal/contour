// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/Blur.h>
#include <contour/display/ScissorRect.h>
#include <contour/display/ShaderConfig.h>

#include <vtbackend/Image.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <crispy/StrongHash.h>

#include <QtGui/QMatrix4x4>
#include <QtOpenGL/QOpenGLPixelTransferOptions>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <memory>
#include <optional>

#include <rhi/qrhi.h>

namespace contour::display
{

/// Deleter that destroys a QRhi resource through its virtual destructor.
///
/// QRhi factory methods (QRhi::newBuffer, newTexture, ...) hand back ownership as a raw pointer; the
/// resource is released with @c delete (which routes through QRhiResource's virtual destructor and frees
/// the backing GPU object). Wrapping the pointers in std::unique_ptr with this deleter keeps the renderer's
/// resource ownership explicit and exception-safe without a manual teardown list.
struct QRhiResourceDeleter
{
    /// Deletes the owned QRhi resource.
    /// @param resource The QRhi resource to delete (may be nullptr).
    void operator()(QRhiResource* resource) const noexcept { delete resource; }
};

/// Owning smart pointer for a QRhi resource of type @p T.
template <typename T>
using QRhiResourcePtr = std::unique_ptr<T, QRhiResourceDeleter>;

/// Holds the GPU pipeline state for one draw pass (background-rect or text-glyph).
///
/// Each pass owns its graphics pipeline, the shader-resource-binding set it was built against, the
/// per-frame interleaved vertex buffer the geometry is uploaded into, and the std140 uniform buffer
/// feeding both shader stages. The text pass additionally references the atlas texture + sampler through
/// its bindings (the texture/sampler themselves live on the renderer, since the atlas is shared and
/// re-created on resize). The vertex buffer is an Immutable buffer (re)created on demand when the frame's
/// geometry grows; the uniform buffer is Dynamic and rewritten every frame.
struct RhiPipeline
{
    QRhiResourcePtr<QRhiGraphicsPipeline> pipeline; ///< The graphics pipeline (topology, blend, shaders).
    QRhiResourcePtr<QRhiShaderResourceBindings>
        srb;                                   ///< Uniform/sampler bindings the pipeline was built with.
    QRhiResourcePtr<QRhiBuffer> vertexBuffer;  ///< Immutable per-frame interleaved vertex buffer.
    QRhiResourcePtr<QRhiBuffer> uniformBuffer; ///< Dynamic std140 uniform buffer (shared by both stages).
};

class RhiRenderer final: public vtrasterizer::RenderTarget, public vtrasterizer::atlas::AtlasBackend
{
    using ImageSize = vtbackend::ImageSize;

    using AtlasTextureScreenshot = vtrasterizer::AtlasTextureScreenshot;

    using AtlasTileID = vtrasterizer::atlas::AtlasTileID;

    using ConfigureAtlas = vtrasterizer::atlas::ConfigureAtlas;
    using UploadTile = vtrasterizer::atlas::UploadTile;
    using RenderTile = vtrasterizer::atlas::RenderTile;

  public:
    /**
     * @param targetSurfaceSize Initial render target size in pixels (the size that can be rendered to).
     * @param textureTileSize   Size in pixels for each tile. This should be the grid cell size.
     * @param margin            Page margin.
     */
    RhiRenderer(vtbackend::ImageSize targetSurfaceSize,
                vtbackend::ImageSize textureTileSize,
                vtrasterizer::PageMargin margin);

    ~RhiRenderer() override;

    /// (Re)builds the RHI graphics pipelines, shader resource bindings and GPU buffers for both render
    /// passes against the scene graph's live RHI and render pass.
    ///
    /// Invoked from the render node's prepare() once the QRhi and the frame's render-pass descriptor are
    /// known. The pipelines bake in the render-pass descriptor and the vertex-input/blend state, so they
    /// must be rebuilt whenever the render target's pass layout changes. The atlas texture and sampler are
    /// created here too (so the text pass's bindings can reference them) but the atlas pixel data is
    /// (re)uploaded lazily via the scheduled-execution path.
    /// @param rhi    The scene graph's RHI instance (non-owning; valid for the lifetime of the frame).
    /// @param rpDesc The render-pass descriptor of the current render target (non-owning).
    void createPipelines(QRhi* rhi, QRhiRenderPassDescriptor* rpDesc);

    /// @return true once createPipelines() has built the RHI resources successfully.
    [[nodiscard]] bool pipelinesReady() const noexcept
    {
        return _rectPipeline.pipeline != nullptr && _textPipeline.pipeline != nullptr;
    }

    /// Hands the per-frame RHI submission handles to the renderer for the current frame.
    ///
    /// The render node calls this from its prepare() phase (before the scene graph begins the render pass),
    /// so the subsequent staging (execute(), driven by the terminal render) can queue resource uploads onto
    /// the command buffer, and recordDraws() (called later from render(), inside the pass) can issue draws
    /// against the same command buffer and render target. The handles are non-owning and valid only for the
    /// duration of the frame; recordDraws() clears them once the draws are recorded.
    /// @param rhi The scene graph's RHI instance.
    /// @param cb  The command buffer to queue resource updates onto and record draws into.
    /// @param rt  The render target the pass renders into (supplies the pixel viewport).
    void beginFrame(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* rt)
    {
        _rhi = rhi;
        _commandBuffer = cb;
        _frameRenderTarget = rt;

        // Reset the per-frame draw accumulation. execute() may be called several times before flushFrame().
        _frameRectVertices.clear();
        _frameTextVertices.clear();
        _frameDrawItems.clear();
        _frameUpdates = nullptr;

        // Start the frame with no transient inner scissor, so a clip left over from a previous frame's
        // smooth-scroll pass cannot leak into this frame's draw items.
        _innerScissor.reset();
    }

    /// Flushes the frame's accumulated geometry/atlas uploads onto the command buffer, ending the staging
    /// phase.
    ///
    /// Called from the render node's prepare() after the terminal render (which may invoke execute() several
    /// times). Uploads the accumulated rect/text vertex buffers and queues the per-frame resource-update
    /// batch (vertex/uniform writes + atlas uploads) onto the command buffer via cb->resourceUpdate(), so it
    /// is processed before the scene graph begins the render pass. After this, recordDraws() replays the
    /// queued draw items.
    void flushFrame();

    /// Records the staged frame's draw commands into the command buffer, inside the active render pass.
    ///
    /// Called from the render node's render() (which runs inside the scene graph's render pass), after the
    /// prepare() phase staged the geometry/atlas and flushFrame() uploaded it. Replays the queued draw items
    /// (rect/text pipeline draws, each with its captured scissor intersected with the node clip) so the
    /// terminal composites in z-order. No-op if nothing was staged this frame.
    void recordDraws();

    // AtlasBackend implementation
    [[nodiscard]] ImageSize atlasSize() const noexcept override;
    void configureAtlas(ConfigureAtlas atlas) override;
    void uploadTile(UploadTile tile) override;
    void renderTile(RenderTile tile) override;

    // RenderTarget implementation
    void setRenderSize(vtbackend::ImageSize targetSurfaceSize) override;
    [[nodiscard]] vtbackend::ImageSize renderSize() const noexcept override { return _renderTargetSize; }
    void setModelMatrix(QMatrix4x4 const& matrix) noexcept;
    void setMargin(vtrasterizer::PageMargin margin) noexcept override;
    std::optional<AtlasTextureScreenshot> readAtlas() override;
    AtlasBackend& textureScheduler() override;
    void scheduleScreenshot(ScreenshotCallback callback) override;
    void renderRectangle(int x, int y, Width, Height, RGBAColor color) override;
    void setScissorRect(int x, int y, int width, int height) override;
    void clearScissorRect() override;

    /// Sets the full item-local→clip transform supplied by the Qt scene graph (projection * node matrix).
    ///
    /// Replaces the renderer's own window-spanning ortho + view translation: when rendering through a
    /// QSGRenderNode, Qt owns placement (including device-pixel ratio and Y orientation), so the renderer
    /// feeds its item-local pixel vertices through this single matrix. The model matrix (QML transforms)
    /// is still applied on top of it in execute().
    /// @param matrix The combined projection * node-matrix from QSGRenderNode::RenderState.
    void setProjectionMatrix(QMatrix4x4 const& matrix) noexcept;

    /// Installs the scene graph's clip rectangle for the frame (bottom-left-origin device pixels) and
    /// enables the scissor test, so the transient inner scissor (smooth scroll / cursor) intersects
    /// against Qt's clip rather than a hand-rolled title-bar rectangle. clearScissorRect() restores this
    /// node clip rather than disabling the scissor test outright. Pass std::nullopt when Qt is not
    /// clipping the node.
    /// @param rect The node clip in OpenGL scissor coordinates, or std::nullopt for no clip.
    void setNodeScissorRect(std::optional<ScissorRect> const& rect);

    /// Forgets the installed node clip WITHOUT touching GL state. Called after the node has rendered: the
    /// scene graph re-establishes its own scissor afterwards (the node declares ScissorState), so emitting
    /// a glDisable here would be redundant churn. Only the stored member is cleared, so no later code path
    /// intersects against a stale rectangle.
    void clearNodeScissorRect() noexcept;
    void execute(std::chrono::steady_clock::time_point now) override;

    std::pair<vtbackend::ImageSize, std::vector<uint8_t>> takeScreenshot();

    void clearCache() override;

    void inspect(std::ostream& output) const override;

    void setTextOutline(float thickness, vtbackend::RGBAColor color) override;

    float uptime(std::chrono::steady_clock::time_point now) noexcept
    {
        using namespace std::chrono;
        auto const uptimeMsecs = duration_cast<milliseconds>(now - _startTime).count();
        auto const uptimeSecs = static_cast<float>(uptimeMsecs) / 1000.0f;
        return uptimeSecs;
    }

    [[nodiscard]] constexpr bool initialized() const noexcept { return _initialized; }

  public slots:
    void initialize();

  private:
    // private helper methods
    //

    /// Loads a baked .qsb shader blob from the Qt resource system and deserializes it into a QShader.
    /// @param resourcePath The qrc path of the .qsb blob (e.g. ":/contour/display/shaders/text.vert.qsb").
    /// @return The deserialized shader; an invalid QShader if the resource could not be read.
    [[nodiscard]] static QShader loadShader(QString const& resourcePath);

    /// Builds the background/rect graphics pipeline (no texture; one uniform block).
    /// @param rhi    The scene graph's RHI instance.
    /// @param rpDesc The render-pass descriptor to bake into the pipeline.
    void createRectPipeline(QRhi* rhi, QRhiRenderPassDescriptor* rpDesc);

    /// Builds the text/glyph graphics pipeline (atlas sampler at binding 1; one uniform block).
    /// @param rhi    The scene graph's RHI instance.
    /// @param rpDesc The render-pass descriptor to bake into the pipeline.
    void createTextPipeline(QRhi* rhi, QRhiRenderPassDescriptor* rpDesc);

    /// Creates (or re-creates) the glyph atlas texture and its sampler for the given size.
    /// @param rhi  The scene graph's RHI instance.
    /// @param size The atlas texture size in pixels (power of two).
    void createAtlasTexture(QRhi* rhi, ImageSize size);

    /// Appends one execute() call's filled-rect (background) geometry to the per-frame accumulator.
    ///
    /// Copies the current _rectBuffer into _frameRectVertices and records a FrameDrawItem (vertex range +
    /// the active inner scissor) so recordDraws() can replay it. No-op when no rectangles were queued.
    void recordRectPass();

    /// Appends one execute() call's text/glyph geometry to the per-frame accumulator.
    ///
    /// Copies the current text render batch into _frameTextVertices and records a FrameDrawItem (vertex
    /// range + the active inner scissor) so recordDraws() can replay it. No-op when no tiles were queued.
    void recordTextPass();

    /// Applies the captured clip to the command buffer for the pipeline about to be drawn, mapping the
    /// bottom-left-origin ScissorRect to QRhiScissor.
    /// @param pipeline    The pipeline being drawn (only scissored if it declares UsesScissor).
    /// @param innerScissor The transient inner scissor captured for this draw, intersected with the node
    ///                     clip here; std::nullopt for no inner scissor.
    void applyScissor(QRhiGraphicsPipeline* pipeline, std::optional<ScissorRect> const& innerScissor);

    void executeConfigureAtlas(ConfigureAtlas const& param);
    void executeUploadTile(QRhiResourceUpdateBatch& updates, UploadTile const& param);

    // -------------------------------------------------------------------------------------------
    // private data members
    //

    // {{{ scheduling data
    struct RenderBatch
    {
        std::vector<vtrasterizer::atlas::RenderTile> renderTiles;
        std::vector<float> buffer;
        uint32_t userdata = 0;

        void clear()
        {
            renderTiles.clear();
            buffer.clear();
        }
    };

    struct Scheduler
    {
        std::optional<vtrasterizer::atlas::ConfigureAtlas> configureAtlas = std::nullopt;
        std::vector<vtrasterizer::atlas::UploadTile> uploadTiles {};
        RenderBatch renderBatch {};

        void clear()
        {
            configureAtlas.reset();
            uploadTiles.clear();
            renderBatch.clear();
        }
    };

    Scheduler _scheduledExecutions;
    // }}}

    bool _initialized = false;
    std::chrono::steady_clock::time_point _startTime;
    vtbackend::ImageSize _renderTargetSize;
    /// The scene graph's item-local→clip transform (projection * node matrix), set per frame from
    /// QSGRenderNode::RenderState; subsumes the former separate view (item translation) matrix.
    QMatrix4x4 _projectionMatrix;
    QMatrix4x4 _modelMatrix;

    // The scene graph's clip rectangle for the current frame (bottom-left-origin device pixels), set from
    // QSGRenderNode::RenderState. The transient inner scissor intersects against it, and clearScissorRect()
    // restores it. std::nullopt when Qt is not clipping the node.
    std::optional<ScissorRect> _nodeScissor;

    // The transient inner scissor (smooth scroll / cursor clip), already intersected with the node clip,
    // staged by setScissorRect()/clearScissorRect() so Phase 3 can map it to QRhiCommandBuffer::setScissor.
    // std::nullopt means "use the node clip / full node area".
    std::optional<ScissorRect> _innerScissor;

    QOpenGLPixelTransferOptions _transferOptions;

    vtrasterizer::PageMargin _margin {};

    vtbackend::RGBAColor _textOutlineColor { 0x00, 0x00, 0x00, 0xFF };

    // The RHI instance the current pipelines were built against. Non-owning; owned by the scene graph.
    // Used to detect when the pipelines must be rebuilt (different RHI / device loss) and to allocate
    // resource-update batches at submission time.
    QRhi* _rhi = nullptr;

    // The render-pass descriptor the pipelines were baked against. Non-owning. Pipelines must be rebuilt
    // if this changes.
    QRhiRenderPassDescriptor* _renderPassDescriptor = nullptr;

    // Per-frame RHI submission handles, set by beginFrame() from the render node. Non-owning; valid only
    // during a single scene-graph frame (prepare()+render()).
    QRhiCommandBuffer* _commandBuffer = nullptr;
    QRhiRenderTarget* _frameRenderTarget = nullptr;

    // {{{ Per-frame draw accumulation (prepare phase) → replay (render phase)
    //
    // Qt's RHI render-node contract requires resource uploads to be issued *before* the render pass begins
    // (in prepare()) and only draw commands inside the pass (render()); see the Qt "Custom Render Node"
    // example. Complicating this, vtrasterizer's Renderer calls execute() MULTIPLE times per frame in the
    // smooth-scroll path (main content under a scissor, then status line, then cursor under a scissor), each
    // call expecting its geometry to be drawn with its own scissor. The old GL backend drew immediately on
    // each execute(); here we instead ACCUMULATE: each execute() appends its rect/text geometry to a
    // per-frame interleaved buffer and records a FrameDrawItem (which pass, vertex range, scissor). The
    // accumulated vertex buffers + the atlas-upload batch are flushed to the command buffer once, at the end
    // of the prepare phase (flushFrame()), and recordDraws() replays the draw items in order.

    /// One queued draw call captured during the prepare phase, replayed in recordDraws().
    enum class FramePass : uint8_t
    {
        Rect, ///< Background/filled-rect pipeline.
        Text, ///< Text/glyph pipeline (samples the atlas).
    };
    struct FrameDrawItem
    {
        FramePass pass = FramePass::Rect;   ///< Which pipeline to draw with.
        quint32 firstVertex = 0;            ///< First vertex (into the pass's per-frame vertex buffer).
        quint32 vertexCount = 0;            ///< Number of vertices to draw.
        std::optional<ScissorRect> scissor; ///< Transient inner scissor for this draw (raw; pre node-clip).
    };

    std::vector<float> _frameRectVertices;      ///< Accumulated rect-pass vertices for the current frame.
    std::vector<float> _frameTextVertices;      ///< Accumulated text-pass vertices for the current frame.
    std::vector<FrameDrawItem> _frameDrawItems; ///< Ordered draw calls for the current frame.

    // The single resource-update batch accumulated across this frame's execute() calls (vertex/uniform
    // writes and atlas uploads), flushed onto the command buffer once in flushFrame(). Non-owning (owned by
    // the QRhi until consumed by cb->resourceUpdate()).
    QRhiResourceUpdateBatch* _frameUpdates = nullptr;
    // }}}

    // {{{ RHI pipelines and atlas resources
    RhiPipeline _rectPipeline; ///< Background/filled-rect pass.
    RhiPipeline _textPipeline; ///< Text/glyph pass (samples the atlas).

    // The glyph atlas texture (RGBA8) and the sampler used by the text pass. Both are referenced by the
    // text pipeline's shader-resource-bindings. The texture is re-created on configureAtlas when the atlas
    // size changes.
    QRhiResourcePtr<QRhiTexture> _atlasTexture;
    QRhiResourcePtr<QRhiSampler> _atlasSampler;
    ImageSize _atlasTextureSize {};

    // The pixel size the live atlas QRhiTexture was actually created at. Distinct from _atlasTextureSize,
    // which configureAtlas() overwrites with the requested size *before* execute() runs, so it cannot tell
    // executeConfigureAtlas() whether the GPU texture must be recreated. This tracks the real GPU extent so
    // a repeated ConfigureAtlas of the same size reuses the existing texture instead of recreating it.
    ImageSize _atlasCreatedSize {};

    vtrasterizer::atlas::AtlasProperties _atlasProperties {};
    // }}}

    // Deferred atlas readback (debug "dump state" / inspector). A readback scheduled into a frame's resource
    // batch only completes once the command buffer is submitted (after the render pass ends), so the result
    // lands one frame later. readAtlas() requests a capture on the next frame and returns the most recently
    // captured pixels; the first call returns zeros until a frame has completed. Non-blocking by design.
    bool _atlasReadbackRequested = false;
    QRhiReadbackResult _atlasReadbackResult;

    // CPU-side interleaved vertex buffer for the filled-rect (background) pass, populated by
    // renderRectangle() and uploaded/drawn in execute().
    std::vector<float> _rectBuffer;

    std::optional<ScreenshotCallback> _pendingScreenshotCallback;

    // render state cache
    struct
    {
        vtbackend::RGBAColor backgroundColor {};
        float backgroundImageOpacity = 1.0f;
        bool backgroundImageBlur = false;
        QSize backgroundResolution;
        crispy::strong_hash backgroundImageHash {};
    } _renderStateCache;
};

} // namespace contour::display
