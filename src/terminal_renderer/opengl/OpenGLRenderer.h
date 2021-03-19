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
#pragma once

#include <terminal_renderer/RenderTarget.h>
#include <terminal_renderer/Atlas.h>

#include <crispy/debuglog.h>

#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLShaderProgram>

#include <memory>
#include <optional>

namespace terminal::renderer::opengl {

struct ShaderConfig;

class OpenGLRenderer :
    public RenderTarget,
    public QOpenGLExtraFunctions
{
  private:
    struct TextureScheduler;

  public:
    OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                   ShaderConfig const& _rectShaderConfig,
                   int _width,
                   int _height,
                   int _leftMargin,
                   int _bottomMargin);

    ~OpenGLRenderer() override;

    void setRenderSize(int _width, int _height) override;
    void setMargin(int _left, int _bottom) noexcept override;

    atlas::TextureAtlasAllocator& monochromeAtlasAllocator() noexcept override;
    atlas::TextureAtlasAllocator& coloredAtlasAllocator() noexcept override;
    atlas::TextureAtlasAllocator& lcdAtlasAllocator() noexcept override;

    atlas::CommandListener& textureScheduler() override;

    void renderRectangle(unsigned _x, unsigned _y, unsigned _width, unsigned _height,
                         float _r, float _g, float _b, float _a) override;

    void execute() override;

    void clearCache() override;

    std::optional<AtlasTextureInfo> readAtlas(atlas::TextureAtlasAllocator const& _allocator, unsigned _instanceId) override;

  private:
    // private helper methods
    //
    void initialize();
    void initializeTextureRendering();
    void initializeRectRendering();
    unsigned maxTextureDepth();
    unsigned maxTextureSize();
    unsigned maxTextureUnits();

    void selectTextureUnit(unsigned _id);
    void bindTexture2DArray(GLuint _textureId);

    void executeRenderTextures();
    void createAtlas(atlas::CreateAtlas const& _param);
    void uploadTexture(atlas::UploadTexture const& _param);
    void renderTexture(atlas::RenderTexture const& _param);
    void destroyAtlas(atlas::DestroyAtlas const& _param);

    void executeRenderRectangle(unsigned _x, unsigned _y, unsigned _width, unsigned _height, QVector4D const& _color);

    // private helper types
    //
    struct AtlasKey {
        std::reference_wrapper<std::string const> name;
        unsigned atlasTexture;

        bool operator<(AtlasKey const& _rhs) const noexcept
        {
            if (name.get() < _rhs.name.get())
                return true;
            else if (name.get() == _rhs.name.get())
                return atlasTexture < _rhs.atlasTexture;
            else
                return false;
        }
    };

    void clearTexture2DArray(GLuint _textureId, unsigned _width, unsigned _height, atlas::Format _format);

    // -------------------------------------------------------------------------------------------
    // private data members
    //
    bool initialized_ = false;
    QMatrix4x4 projectionMatrix_;

    int leftMargin_ = 0;
    int bottomMargin_ = 0;

    std::unique_ptr<QOpenGLShaderProgram> textShader_;
    int textProjectionLocation_;

    // private data members for rendering textures
    //
    GLuint vao_{};              // Vertex Array Object, covering all buffer objects
    GLuint vbo_{};              // Buffer containing the vertex coordinates
    //TODO: GLuint ebo_{};
    std::map<AtlasKey, GLuint> atlasMap_; // maps atlas IDs to texture IDs
    GLuint currentActiveTexture_ = std::numeric_limits<GLuint>::max();
    GLuint currentTextureId_ = std::numeric_limits<GLuint>::max();
    std::unique_ptr<TextureScheduler> textureScheduler_;
    atlas::TextureAtlasAllocator monochromeAtlasAllocator_;
    atlas::TextureAtlasAllocator coloredAtlasAllocator_;
    atlas::TextureAtlasAllocator lcdAtlasAllocator_;

    // private data members for rendering filled rectangles
    //
    std::vector<GLfloat> rectBuffer_;
    std::unique_ptr<QOpenGLShaderProgram> rectShader_;
    GLint rectProjectionLocation_;
    GLuint rectVAO_;
    GLuint rectVBO_;
};

} // end namespace
