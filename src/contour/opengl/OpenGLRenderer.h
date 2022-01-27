/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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
#pragma once

#include <terminal_renderer/RenderTarget.h>
#include <terminal_renderer/TextureAtlas.h>

#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLExtraFunctions>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGL/QOpenGLShaderProgram>
#else
    #include <QtGui/QOpenGLShaderProgram>
#endif

#include <memory>
#include <optional>
#include <unordered_map>

namespace contour::opengl
{

struct ShaderConfig;

class OpenGLRenderer final:
    public terminal::renderer::RenderTarget,
    public terminal::renderer::atlas::AtlasBackend,
    public QOpenGLExtraFunctions
{
    using ImageSize = terminal::ImageSize;

    using AtlasTextureScreenshot = terminal::renderer::AtlasTextureScreenshot;

    using AtlasTileID = terminal::renderer::atlas::AtlasTileID;

    using ConfigureAtlas = terminal::renderer::atlas::ConfigureAtlas;
    using UploadTile = terminal::renderer::atlas::UploadTile;
    using RenderTile = terminal::renderer::atlas::RenderTile;

  public:
    /**
     * @param renderSize        Sets the render target's size in pixels.
     *                          This is the size that can be rendered to.
     * @param textureAtlasSize  size in pixels for the texture atlas. Must be power of two.
     * @param tileSize          size in pixels for each tile. This should be the grid cell size.
     */
    OpenGLRenderer(ShaderConfig const& textShaderConfig,
                   ShaderConfig const& rectShaderConfig,
                   ShaderConfig const& backgroundImageShaderConfig,
                   crispy::ImageSize renderSize,
                   crispy::ImageSize textureTileSize,
                   terminal::renderer::PageMargin margin);

    ~OpenGLRenderer() override;

    // AtlasBackend implementation
    ImageSize atlasSize() const noexcept override;
    void configureAtlas(ConfigureAtlas atlas) override;
    void uploadTile(UploadTile tile) override;
    void renderTile(RenderTile tile) override;

    // RenderTarget implementation
    void setRenderSize(crispy::ImageSize _size) override;
    void setMargin(terminal::renderer::PageMargin _margin) noexcept override;
    std::optional<AtlasTextureScreenshot> readAtlas() override;
    AtlasBackend& textureScheduler() override;
    void scheduleScreenshot(ScreenshotCallback _callback) override;
    void setBackgroundImage(std::optional<terminal::BackgroundImage> const& _backgroundImage) override;
    void renderRectangle(int x, int y, Width, Height, RGBAColor color) override;
    void clear(terminal::RGBAColor _fillColor) override;
    void execute() override;

    std::pair<crispy::ImageSize, std::vector<uint8_t>> takeScreenshot();

    void clearCache() override;

    void inspect(std::ostream& output) const override;

  private:
    // private helper methods
    //
    void initialize();
    void initializeBackgroundRendering();
    void initializeTextureRendering();
    void initializeRectRendering();
    int maxTextureDepth();
    int maxTextureSize();
    int maxTextureUnits();
    crispy::ImageSize renderBufferSize();

    void executeRenderBackground();
    void executeRenderTextures();
    void executeConfigureAtlas(ConfigureAtlas const& _param);
    void executeUploadTile(UploadTile const& _param);
    void executeRenderTile(RenderTile const& _param);
    void executeDestroyAtlas();

    //? void renderRectangle(int _x, int _y, int _width, int _height, QVector4D const& _color);

    void bindTexture(GLuint _textureId);

    // -------------------------------------------------------------------------------------------
    // private data members
    //

    // {{{ scheduling data
    struct RenderBatch
    {
        std::vector<terminal::renderer::atlas::RenderTile> renderTiles;
        std::vector<GLfloat> buffer;
        uint32_t userdata = 0;

        void clear()
        {
            renderTiles.clear();
            buffer.clear();
        }
    };

    struct Scheduler
    {
        std::optional<terminal::renderer::atlas::ConfigureAtlas> configureAtlas = std::nullopt;
        std::vector<terminal::renderer::atlas::UploadTile> uploadTiles {};
        RenderBatch renderBatch {};
        terminal::BackgroundImage const* backgroundImage = nullptr;

        void clear()
        {
            configureAtlas.reset();
            uploadTiles.clear();
            renderBatch.clear();
            backgroundImage = nullptr;
        }
    };

    Scheduler _scheduledExecutions;
    // }}}

    bool _initialized = false;
    crispy::ImageSize _renderTargetSize;
    QMatrix4x4 _projectionMatrix;

    terminal::renderer::PageMargin _margin {};

    std::unique_ptr<QOpenGLShaderProgram> _textShader;
    int _textProjectionLocation;

    // private data members for rendering textures
    //
    GLuint _textVAO {}; // Vertex Array Object, covering all buffer objects
    GLuint _textVBO {}; // Buffer containing the vertex coordinates
    // TODO: GLuint ebo_{};

    // currently bound texture ID during execution
    GLuint _currentTextureId = std::numeric_limits<GLuint>::max();

    // background / background-image related fields
    GLuint _backgroundVAO {};
    GLuint _backgroundVBO {};
    GLuint _backgroundImageTexture {};
    std::unique_ptr<QOpenGLShaderProgram> _backgroundShader;
    struct
    {
        int projection;
        int resolution;
        int blur;
        int opacity;
        int time;
    } _backgroundUniformLocations {};

    // index equals AtlasID
    struct AtlasAttributes
    {
        GLuint textureId {};
        ImageSize textureSize {};
        terminal::renderer::atlas::AtlasProperties properties {};
    };
    AtlasAttributes _textureAtlas {};

    // private data members for rendering filled rectangles
    //
    std::vector<GLfloat> _rectBuffer;
    std::unique_ptr<QOpenGLShaderProgram> _rectShader;
    GLint _rectProjectionLocation;
    GLuint _rectVAO;
    GLuint _rectVBO;

    std::optional<ScreenshotCallback> _pendingScreenshotCallback;

    // render state cache
    struct
    {
        terminal::RGBAColor backgroundColor {};
        float backgroundImageOpacity = 1.0f;
        terminal::BackgroundImage const* backgroundImage = nullptr;
        crispy::StrongHash backgroundImageHash;
    } _renderStateCache;
};

} // namespace contour::opengl
