// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/ScissorRect.h>
#include <contour/display/ShaderConfig.h>

#include <vtbackend/Image.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <crispy/StrongHash.h>

#include <QtCore/QVarLengthArray>
#include <QtGui/QMatrix4x4>
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

class RhiRenderer final:
    public vtrasterizer::RenderTarget,
    public vtrasterizer::atlas::AtlasBackend,
    public vtrasterizer::atlas::ImageTextureBackend
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
     */
    RhiRenderer(vtbackend::ImageSize targetSurfaceSize, vtbackend::ImageSize textureTileSize);

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

    // ImageTextureBackend implementation
    void createImageTexture(vtrasterizer::atlas::CreateImageTexture param) override;
    void destroyImageTexture(vtrasterizer::atlas::DestroyImageTexture param) override;
    void renderImageQuad(vtrasterizer::atlas::RenderImageQuad param) override;
    void renderImageGap(vtrasterizer::atlas::RenderImageGap param) override;
    [[nodiscard]] std::vector<vtrasterizer::atlas::ImageTextureId> takeFailedImageTextures() override;
    vtrasterizer::atlas::ImageTextureBackend& imageScheduler() override;

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

    /// Installs this item's top-left corner inside the swapchain render target, in device pixels
    /// (top-left origin). applyScissor() translates the rasterizer's item-relative inner scissor by this
    /// offset when drawing into the window — a split pane is not at the window's bottom-left, so an
    /// untranslated scissor would clip the wrong pane's region.
    /// @param leftDevicePx The item's left edge, device pixels from the target's left.
    /// @param topDevicePx  The item's top edge, device pixels from the target's top.
    void setItemOriginInTarget(int leftDevicePx, int topDevicePx) noexcept
    {
        _itemOriginLeftDevice = leftDevicePx;
        _itemOriginTopDevice = topDevicePx;
    }
    void execute(std::chrono::steady_clock::time_point now) override;

    /// @return true once a screenshot has been requested (scheduleScreenshot) and is awaiting a captured
    ///         frame; drives the render node to run the offscreen screenshot pass this frame.
    [[nodiscard]] bool screenshotRequested() const noexcept { return _pendingScreenshotCallback.has_value(); }

    /// Renders this frame's staged draw items into an owned offscreen RGBA8 texture and schedules a
    /// deferred readback of it, when a screenshot is pending.
    ///
    /// Called from the render node's prepare() phase (after flushFrame() staged the geometry, and before the
    /// scene graph begins its own render pass — the only point at which no pass is active on the command
    /// buffer, so a fresh beginPass/endPass into the offscreen target is legal). Replaying the same draw
    /// items into an owned texture render target captures the terminal-only pixels — no window chrome. The
    /// offscreen target + a pipeline set built against its own render-pass descriptor are created lazily and
    /// resized on demand. The readback lands one frame later; deliverScreenshot() forwards it to the pending
    /// callback. No-op unless a screenshot is pending and the pipelines are ready.
    /// @param rhi The scene graph's RHI instance.
    /// @param cb  The live command buffer to record the offscreen pass onto.
    void recordScreenshotPass(QRhi* rhi, QRhiCommandBuffer* cb);

    /// Delivers a completed offscreen screenshot readback to the pending callback, if any.
    ///
    /// Called once per frame (from the render node) so a readback scheduled on the previous frame — whose
    /// QRhiReadbackResult has since completed — is handed to scheduleScreenshot()'s callback and the
    /// request cleared. No-op until a capture has completed. Non-blocking by design.
    void deliverScreenshot();

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

    // Forward reference: the pass descriptor accessor is defined out-of-line after the struct.

    /// Describes the per-pass variation between the RHI graphics pipelines (shaders, uniform-block size,
    /// vertex stride + attribute layout, whether the pass samples the glyph atlas). Everything not captured
    /// here (topology, depth/blend/scissor state, the binding-0 uniform buffer) is identical for every pass
    /// and lives directly in createPipeline(). The concrete pass table + backing attribute arrays live in
    /// the .cpp; this is the interpreter contract.
    struct PassDescriptor
    {
        QString vertexShaderPath;     ///< qrc path of the baked vertex shader (.qsb).
        QString fragmentShaderPath;   ///< qrc path of the baked fragment shader (.qsb).
        quint32 uniformBlockSize = 0; ///< std140 uniform block size in bytes (binding 0).
        quint32 vertexStride = 0;     ///< Interleaved vertex stride in bytes (single binding 0).
        std::span<QRhiVertexInputAttribute const> attributes; ///< Vertex-input attributes for this pass.
        bool hasSampler = false;         ///< Whether the pass samples the glyph atlas at binding 1.
        char const* debugName = nullptr; ///< Human-readable pass name for diagnostics.
    };

    /// @return the pass descriptor for the background/rect pass (isText == false) or the text/glyph pass
    ///         (isText == true). The two-row pass table; shared by the swapchain and screenshot pipelines.
    /// @param isText true for the text/glyph pass, false for background/rect.
    [[nodiscard]] static PassDescriptor passDescriptor(bool isText);

    /// Builds one graphics pipeline (+ its SRB) from a pass descriptor into @p out.
    ///
    /// Data-driven replacement for the former per-pass createRectPipeline()/createTextPipeline(): the only
    /// variation is carried by @p desc. A pass that samples the atlas (desc.hasSampler) requires the atlas
    /// texture + sampler to already exist (createPipelines() ensures that ordering).
    /// @param rhi    The scene graph's RHI instance.
    /// @param rpDesc The render-pass descriptor to bake into the pipeline.
    /// @param desc   The pass description (shaders, layout, sampler flag).
    /// @param out    The pipeline slot to populate.
    /// @param sharedUniformBuffer When non-null, the SRB binds this existing uniform buffer instead of the
    ///                            method creating a new one in @p out. Used by the screenshot pipelines to
    ///                            share the swapchain set's per-frame uniforms; pass nullptr for the normal
    ///                            (owning) case.
    void createPipeline(QRhi* rhi,
                        QRhiRenderPassDescriptor* rpDesc,
                        PassDescriptor const& desc,
                        RhiPipeline& out,
                        QRhiBuffer* sharedUniformBuffer = nullptr);

    /// The shader-resource bindings shared by every pass: the uniform block at binding 0 (both stages),
    /// plus the atlas sampler at binding 1 (fragment stage) for sampling passes. Single source of truth for
    /// the binding layout, used both when creating a pipeline's SRB and when re-pointing it at a recreated
    /// atlas texture, so the two never drift.
    /// @param uniformBuffer The uniform buffer to bind at slot 0.
    /// @param hasSampler    Whether to add the atlas sampler at slot 1 (true for the text/glyph pass).
    /// @return The 1- or 2-element binding list.
    [[nodiscard]] QVarLengthArray<QRhiShaderResourceBinding, 2> atlasSrbBindings(QRhiBuffer* uniformBuffer,
                                                                                 bool hasSampler) const;

    /// Creates (or re-creates) the glyph atlas texture and its sampler for the given size.
    /// @param rhi  The scene graph's RHI instance.
    /// @param size The atlas texture size in pixels (power of two).
    void createAtlasTexture(QRhi* rhi, ImageSize size);

    /// Re-points a text pipeline's shader-resource bindings (binding 1) at the current _atlasTexture.
    ///
    /// Every SRB that samples the glyph atlas holds a reference to a specific QRhiTexture object; when
    /// createAtlasTexture() replaces _atlasTexture (destroying the old one), every such SRB must be rebound
    /// or it samples a freed texture. executeConfigureAtlas() calls this for each atlas-sampling pipeline
    /// after a recreate.
    /// @param pipeline      The pipeline whose srb references the atlas (no-op if it has no srb).
    /// @param uniformBuffer The uniform buffer bound at slot 0. The screenshot pipelines share the
    ///                      swapchain pipeline's buffer (their own uniformBuffer field is null), so it is
    ///                      passed explicitly rather than read from @p pipeline.
    void rebindAtlasTexture(RhiPipeline& pipeline, QRhiBuffer* uniformBuffer);

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
    ///
    /// The inner scissor is staged in ITEM-relative coordinates (vtrasterizer's reference frame is
    /// renderSize(), the item's extent). Drawing into the window it is translated by the item's offset
    /// (setItemOriginInTarget) and intersected with the node clip; drawing into the item-sized offscreen
    /// screenshot target the frames coincide, so it is used untranslated and the (window-space) node clip
    /// does not apply.
    /// @param pipeline    The pipeline being drawn (only scissored if it declares UsesScissor).
    /// @param innerScissor The transient inner scissor captured for this draw; std::nullopt for none.
    /// @param targetPixelSize The active render target's pixel size (frame conversion + full-area clip).
    /// @param offscreen   true when drawing into the item-sized offscreen screenshot target.
    void applyScissor(QRhiGraphicsPipeline* pipeline,
                      std::optional<ScissorRect> const& innerScissor,
                      QSize targetPixelSize,
                      bool offscreen);

    /// Replays the frame's accumulated draw items onto @p cb for @p targetPixelSize, using either the
    /// swapchain pipelines (the live pass) or the offscreen screenshot pipelines. Shared by recordDraws()
    /// (swapchain) and recordScreenshotPass() (offscreen) so the per-item pipeline/vertex/scissor binding
    /// logic exists once.
    /// @param cb              The command buffer to record draws onto (a pass must already be active).
    /// @param targetPixelSize The active render target's pixel size (drives viewport + the full-area clip).
    /// @param offscreen       true to bind the offscreen screenshot pipeline set, false for the swapchain
    /// set.
    void replayDrawItems(QRhiCommandBuffer* cb, QSize targetPixelSize, bool offscreen);

    /// Creates (or resizes) the offscreen screenshot render target — an owned RGBA8 color texture (readback
    /// source) + a depth-stencil buffer + a texture render target with its own render-pass descriptor — and
    /// the screenshot pipeline set built against that descriptor. No-op if it already exists at @p size.
    /// @param rhi  The scene graph's RHI instance.
    /// @param size The capture size in device pixels (the terminal's render-target size).
    /// @return true if the offscreen resources are ready to render into.
    [[nodiscard]] bool ensureScreenshotTarget(QRhi* rhi, ImageSize size);

    /// Builds one screenshot-pass graphics pipeline against the offscreen render-pass descriptor.
    ///
    /// Uses the same pass descriptor (shaders, vertex layout, sampler flag) as the swapchain pipeline and
    /// the shared atlas texture/sampler + vertex buffers, but OWNS its uniform buffer: the offscreen pass
    /// renders into an ITEM-sized target and therefore needs an item-local transform, not the swapchain's
    /// item→window transform (sharing that captured every screenshot shifted by the item's window offset
    /// and scaled by the item/window size ratio). recordScreenshotPass() uploads the screenshot uniforms.
    /// @param rhi    The scene graph's RHI instance.
    /// @param isText true to build the text/glyph pass (adds the atlas sampler), false for background/rect.
    void createScreenshotPipeline(QRhi* rhi, bool isText);

    /// Uploads one pass's per-frame uniform block into @p uniformBuffer: the transform + time, plus the
    /// text pass's pixel_x + outline color. Shared by flushFrame() (swapchain buffers, item→window
    /// transform) and recordScreenshotPass() (offscreen buffers, item-local transform), so the block
    /// layout is written in exactly one place per pass.
    /// @param updates       The resource-update batch to queue the writes onto.
    /// @param uniformBuffer The destination uniform buffer (swapchain or screenshot set).
    /// @param mvp           The vertex transform for this target.
    /// @param timeValue     Uptime in seconds (drives shader animation).
    void uploadRectUniforms(QRhiResourceUpdateBatch& updates,
                            QRhiBuffer& uniformBuffer,
                            QMatrix4x4 const& mvp,
                            float timeValue);
    /// @copydoc uploadRectUniforms
    void uploadTextUniforms(QRhiResourceUpdateBatch& updates,
                            QRhiBuffer& uniformBuffer,
                            QMatrix4x4 const& mvp,
                            float timeValue);

    void executeConfigureAtlas(ConfigureAtlas const& param);
    void executeUploadTile(QRhiResourceUpdateBatch& updates, UploadTile const& param);

    /// One run of quads sampling a single image texture.
    ///
    /// A run rather than a quad, because a full-screen image is thousands of quads that all sample
    /// the same texture: one draw item per run instead of one per cell.
    /// One run of image geometry, in issue order within its side of the text.
    ///
    /// Either a run of quads sampling @c texture, or -- when @c texture is unset -- a run of solid gap
    /// fills drawn with the rect pipeline. Both live in one ordered list because both composite by draw
    /// order alone (every vertex sits at the same depth), so a gap fill can only occlude what precedes
    /// it if it keeps its place among the quads.
    struct ImageQuadBatch
    {
        std::optional<vtrasterizer::atlas::ImageTextureId> texture;
        std::vector<float> buffer;
    };

    /// Creates the texture for one whole image and queues its pixel upload.
    ///
    /// The binding sets are left to refreshImageShaderResources(), which is what keeps them agreeing
    /// with pipelines whose uniform buffers outlive no image.
    void executeCreateImageTexture(QRhiResourceUpdateBatch& updates,
                                   vtrasterizer::atlas::CreateImageTexture& param);

    /// Builds the shader-resource set one image quad is drawn with.
    /// @param texture the image's texture; named by binding 1.
    /// @param uniformBuffer the drawing pipeline's uniform buffer; named by binding 0.
    /// @return the new set, or nullptr if it could not be created.
    [[nodiscard]] QRhiShaderResourceBindings* createImageSrb(QRhiTexture* texture, QRhiBuffer* uniformBuffer);

    /// Rebuilds every image binding set that does not name the uniform buffer its pipeline now owns.
    ///
    /// Cheap and idempotent: a set that already names the right buffer is left alone, so the common
    /// frame does no work at all. Must run outside a render pass, and after the pipeline whose
    /// buffer the sets name has been created.
    /// @param offscreen refresh the screenshot sets rather than the swapchain sets.
    void refreshImageShaderResources(bool offscreen);

    /// Appends one draw item per image-texture run of @p batches to the frame.
    void recordImagePass(std::vector<ImageQuadBatch> const& batches);

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
        std::vector<vtrasterizer::atlas::CreateImageTexture> imageCreates {};
        std::vector<vtrasterizer::atlas::DestroyImageTexture> imageDestroys {};
        /// Image quads that composite under the text, and over it. Two lists rather than a layer tag
        /// per quad: the pass order [rect][image-below][text][image-above] is what expresses z here,
        /// since every vertex sits at the same depth and only draw order composites.
        std::vector<ImageQuadBatch> imageQuadsBelowText {};
        std::vector<ImageQuadBatch> imageQuadsAboveText {};

        void clear()
        {
            configureAtlas.reset();
            uploadTiles.clear();
            renderBatch.clear();
            imageCreates.clear();
            imageDestroys.clear();
            imageQuadsBelowText.clear();
            imageQuadsAboveText.clear();
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

    // This item's top-left corner inside the swapchain render target, in device pixels (top-left origin),
    // set per frame by setItemOriginInTarget(). applyScissor() translates the item-relative inner scissor
    // by it when drawing into the window (a split pane is not at the window's bottom-left).
    int _itemOriginLeftDevice = 0;
    int _itemOriginTopDevice = 0;

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
        Rect,  ///< Background/filled-rect pipeline.
        Text,  ///< Text/glyph pipeline (samples the atlas).
        Image, ///< Text pipeline, but sampling one whole-image texture instead of the atlas.
    };
    struct FrameDrawItem
    {
        FramePass pass = FramePass::Rect;   ///< Which pipeline to draw with.
        quint32 firstVertex = 0;            ///< First vertex (into the pass's per-frame vertex buffer).
        quint32 vertexCount = 0;            ///< Number of vertices to draw.
        std::optional<ScissorRect> scissor; ///< Transient inner scissor for this draw (raw; pre node-clip).
        /// For FramePass::Image: which image texture this draw samples. Images share the text
        /// pipeline and its vertex layout — only the bound texture differs — so the pass carries the
        /// texture rather than owning a pipeline of its own.
        vtrasterizer::atlas::ImageTextureId imageTexture {};
    };

    /// One image's shader-resource set, together with the uniform buffer it names.
    ///
    /// A set names two things with very different lifetimes: binding 1 is the image's own texture,
    /// which lives exactly as long as the image, and binding 0 is the drawing pipeline's uniform
    /// buffer, which does not. createPipeline() replaces that buffer whenever the QRhi or the
    /// render-pass descriptor changes, and the screenshot pipelines do not exist at all until the
    /// first capture -- so a set built once and kept is eventually either stale or absent.
    /// Recording what the set was built against is what lets it be rebuilt exactly when it must be.
    struct ImageShaderResources
    {
        QRhiResourcePtr<QRhiShaderResourceBindings> srb;
        /// The uniform buffer @c srb names at binding 0. Not owned, and never dereferenced -- only
        /// compared against the buffer the pipeline currently owns.
        QRhiBuffer* uniformBuffer = nullptr;
    };

    /// GPU resources backing one whole image.
    ///
    /// Two binding sets, because the on-screen and off-screen (screenshot) pipelines feed different
    /// uniform buffers: binding 0 must name the right one, binding 1 this image's texture.
    struct ImageTextureResources
    {
        QRhiResourcePtr<QRhiTexture> texture;
        ImageShaderResources swapchain; ///< Bound when drawing into the swapchain.
        ImageShaderResources offscreen; ///< Bound when replaying into the screenshot target.
        vtbackend::ImageSize size;
    };
    std::unordered_map<uint32_t, ImageTextureResources> _imageTextures;

    /// Ids whose queued creation failed, awaiting collection by takeFailedImageTextures().
    std::vector<vtrasterizer::atlas::ImageTextureId> _failedImageTextures;

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

    // {{{ Deferred offscreen screenshot capture
    //
    // A screenshot renders the terminal's draw items into an owned RGBA8 texture (not the window backbuffer,
    // which would include the title bar / tab strip / split chrome) and reads it back. Like the atlas
    // readback, a texture readback scheduled into a frame only completes after that frame is submitted, so
    // the pixels arrive one frame later and are delivered to the pending callback then. The offscreen target
    // + its pipeline set are created lazily (first screenshot) and resized when the capture size changes.
    std::optional<ScreenshotCallback> _pendingScreenshotCallback;
    QRhiReadbackResult _screenshotReadbackResult;
    ImageSize _screenshotSize {};            ///< Device-pixel size the pending/last capture was taken at.
    bool _screenshotReadbackPending = false; ///< A readback is scheduled and awaiting completion delivery.

    /// Whether the pending capture's readback rows arrive bottom-up and must be reversed to yield a
    /// top-left-origin image. Recorded with the capture it describes (recordScreenshotPass, from
    /// QRhi::isYUpInFramebuffer()), since the row order is a property of the frame that produced it.
    bool _screenshotFlipRows = false;

    QRhiResourcePtr<QRhiTexture> _screenshotTexture; ///< Owned RGBA8 color target (readback source).
    QRhiResourcePtr<QRhiRenderBuffer> _screenshotDepthStencil; ///< Depth-stencil for the offscreen pass.
    QRhiResourcePtr<QRhiTextureRenderTarget> _screenshotRenderTarget; ///< The offscreen render target.
    QRhiResourcePtr<QRhiRenderPassDescriptor> _screenshotRpDesc;      ///< Its render-pass descriptor.
    RhiPipeline _screenshotRectPipeline; ///< Rect pass built against the offscreen render-pass desc.
    RhiPipeline _screenshotTextPipeline; ///< Text pass built against the offscreen render-pass desc.
    // }}}

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
