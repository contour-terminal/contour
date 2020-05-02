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
class Renderer : public QOpenGLExtraFunctions {
  private:
    struct ExecutionScheduler;

  public:
    Renderer();
    ~Renderer();

    unsigned maxTextureDepth();
    unsigned maxTextureSize();

    void setProjection(QMatrix4x4 const& _projection);

    /// Schedules (orders and prepares) a list of Atlas commands for execution.
    void schedule(std::vector<Command> const& _commands);

    /// Schedules (orders and prepares) a list of Atlas commands for execution.
    void schedule(std::vector<DestroyAtlas> const& _commands);

    /// Executes all prepared pending commands in proper order.
    ///
    /// First, schedule commands in order to prepare and fill command queue, then execute.
    void execute();

  private:
    void createAtlas(CreateAtlas const& _atlas);
    void uploadTexture(UploadTexture const& _texture);
    void renderTexture(RenderTexture const& _render);
    void destroyAtlas(DestroyAtlas const& _atlas);

    void bindTexture2DArray(GLuint _textureId);
    void setActiveTexture(unsigned _id);

  private:
    GLuint vao_;                // Vertex Array Object, covering all buffer objects
    GLuint vbo_;                // Buffer containing the vertex coordinates
    GLuint texCoordsBuffer_;    // Buffer containing the texture coordinates
    GLuint texIdBuffer_;        // Buffer containing the texture IDs

    std::unique_ptr<ExecutionScheduler> scheduler_;

    std::map<unsigned, GLuint> atlasMap_{}; // maps atlas IDs to texture IDs

    GLuint currentActiveTexture_ = 0;
    GLuint currentTextureId_ = 0;

    QMatrix4x4 projection_ = {};
};

} // end namespace
