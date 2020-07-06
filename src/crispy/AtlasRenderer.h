/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

#include <crispy/Atlas.h>

#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLTexture>

#include <limits>
#include <algorithm>
#include <memory>

namespace crispy::atlas {

/**
 * Stateful Texture Atlas Renderer.
 *
 * The main goal behind this renderer is especially to minimize the number of OpenGL calls.
 * Therefore, once all commands have pumped into the rendere, the flush() method must be called
 * to make sure any queued render calls will be flushed out to the graphics card.
 */
class Renderer : public QOpenGLExtraFunctions, public CommandListener {
  private:
    struct ExecutionScheduler;

  public:
    Renderer();
    ~Renderer();

    unsigned maxTextureDepth();
    unsigned maxTextureSize();
    unsigned maxTextureUnits();

    void setProjection(QMatrix4x4 const& _projection);

    /// @return an interface to be used to schedule render commands.
    CommandListener& scheduler() noexcept;

    /// Executes all prepared pending commands in proper order.
    ///
    /// First, schedule commands in order to prepare and fill command queue, then execute.
    void execute();

    size_t size() const noexcept;
    bool empty() const noexcept;

  private:
    void createAtlas(CreateAtlas const& _atlas) override;
    void uploadTexture(UploadTexture const& _texture) override;
    void renderTexture(RenderTexture const& _render) override;
    void destroyAtlas(DestroyAtlas const& _atlas) override;

    void selectTextureUnit(unsigned _id);
    void bindTexture2DArray(GLuint _textureId);

  private:
    GLuint vao_;                // Vertex Array Object, covering all buffer objects
    GLuint vbo_;                // Buffer containing the vertex coordinates
    GLuint ebo_;
    GLuint texCoordsBuffer_;    // Buffer containing the texture coordinates
    GLuint colorsBuffer_;       // Buffer containing the text colors

    std::unique_ptr<ExecutionScheduler> scheduler_;

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

    std::map<AtlasKey, GLuint> atlasMap_{}; // maps atlas IDs to texture IDs

    GLuint currentActiveTexture_ = std::numeric_limits<GLuint>::max();
    GLuint currentTextureId_ = std::numeric_limits<GLuint>::max();

    QMatrix4x4 projection_;
};

} // end namespace
