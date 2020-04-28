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

#include <terminal_view/Atlas.h>

#include <QOpenGLTexture>
#include <QOpenGLExtraFunctions>

#include <algorithm>

namespace atlas {

/**
 * Stateful Texture Atlas Renderer.
 *
 * The main goal behind this renderer is especially to minimize the number of OpenGL calls.
 * Therefore, once all commands have pumped into the rendere, the flush() method must be called
 * to make sure any queued render calls will be flushed out to the graphics card.
 */
class Renderer : public QOpenGLExtraFunctions
{
  private:
    struct ExecutionScheduler
    {
        std::vector<CreateAtlas> createAtlases;
        std::vector<UploadTexture> uploadTextures;
        std::vector<RenderTexture> renderTextures;
        std::vector<GLfloat> vecs;
        std::vector<DestroyAtlas> destroyAtlases;

        void operator()(CreateAtlas const& _atlas)
        {
            createAtlases.emplace_back(_atlas);
        }

        void operator()(UploadTexture const& _texture)
        {
            uploadTextures.emplace_back(_texture);
        }

        void operator()(RenderTexture const& _texture)
        {
            renderTextures.emplace_back(_texture);
        }

        void operator()(DestroyAtlas const& _atlas)
        {
            destroyAtlases.push_back(_atlas);
        }

        void clear()
        {
            createAtlases.clear();
            uploadTextures.clear();
            vecs.clear();
            destroyAtlases.clear();
        }
    };

  public:
    Renderer()
    {
        initializeOpenGLFunctions();

        glGenVertexArrays(1, &vao_);

        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &vboTex_);

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);

        glEnableVertexAttribArray(/*index*/ 0);
        glVertexAttribPointer(/*index*/ 0, /*size*/ 0, /*type*/ GL_FLOAT, GL_FALSE, 0, nullptr);

        glEnableVertexAttribArray(/*index*/ 1);
        glVertexAttribPointer(/*index*/ 1, /*size*/ 0, /*type*/ GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    ~Renderer()
    {
        for ([[maybe_unused]] auto [_, textureId] : atlasMap_)
            glDeleteTextures(1, &textureId);

        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
    }

    void schedule(CommandList const& _commands)
    {
        for (Command const& command : _commands)
            visit(scheduler_, command);
    }

    /// Executes all prepared commands in proper order
    ///
    /// First call execute(CommandList&) in order to prepare and fill command queue.
    void execute()
    {
        using namespace std;
        using namespace std::placeholders;

        // potentially create new atlases
        for_each(scheduler_.createAtlases.begin(),
                 scheduler_.createAtlases.end(),
                 bind(&Renderer::createAtlas, this, _1));

        // potentially upload any new textures
        for_each(scheduler_.uploadTextures.begin(),
                 scheduler_.uploadTextures.end(),
                 bind(&Renderer::uploadTexture, this, _1));

        // order and prepare texture geometry
        sort(scheduler_.renderTextures.begin(),
             scheduler_.renderTextures.end(),
             [](RenderTexture const& a, RenderTexture const& b) { return a.atlas < b.atlas; });

        for_each(scheduler_.renderTextures.begin(),
                 scheduler_.renderTextures.end(),
                 bind(&Renderer::renderTexture, this, _1));

        // upload vertices and render (iff there is anything to render)
        if (!scheduler_.renderTextures.empty())
        {
            // upload vertices
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER,
                         scheduler_.vecs.size() * sizeof(GLfloat),
                         scheduler_.vecs.data(),
                         GL_STATIC_DRAW);

            // flush render
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, scheduler_.vecs.size() * 6);

            // TODO: Instead of on glDrawArrays (and many if's in the shader for each GL_TEXTUREi),
            //       make a loop over each GL_TEXTUREi and draw a sub range of the vertices and a
            //       fixed GL_TEXTURE0. - will this be noticable faster?
        }

        // destroy any pending atlases that were meant to be destroyed
        for_each(scheduler_.destroyAtlases.begin(),
                 scheduler_.destroyAtlases.end(),
                 bind(&Renderer::destroyAtlas, this, _1));

        // reset execution state
        scheduler_.clear();
    }

    void execute(CommandList const& _commands)
    {
        schedule(_commands);
        execute();
    }

  private:
    void createAtlas(CreateAtlas const& _atlas)
    {
        constexpr GLuint internalFormat = GL_RED; // TODO: configurable

        GLuint textureId{};

        glGenTextures(1, &textureId);
        bindTexture2DArray(textureId);

        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, internalFormat, _atlas.width, _atlas.height, _atlas.depth);

        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        atlasMap_[_atlas.atlas] = textureId;
    }

    void uploadTexture(UploadTexture const& _texture)
    {
        auto const textureId = atlasMap_[_texture.atlas];
        auto const x0 = _texture.x;
        auto const y0 = _texture.y;
        auto const z0 = _texture.z;
        auto const internalFormat = GL_RED; // TODO: configure me

        auto constexpr target = GL_TEXTURE_2D_ARRAY;
        auto constexpr levelOfDetail = 0;
        auto constexpr depth = 1;
        auto constexpr type = GL_UNSIGNED_BYTE;

        bindTexture2DArray(textureId);

        glTexSubImage3D(target, levelOfDetail, x0, y0, z0, _texture.width, _texture.height, depth,
                        internalFormat, type, _texture.data.data());
    }

    void renderTexture(RenderTexture const& _texture)
    {
        if (auto const it = atlasMap_.find(_texture.atlas); it != atlasMap_.end())
        {
            GLuint const textureId = it->second;
            setActiveTexture(_texture.atlas);
            bindTexture2DArray(textureId);

            // TODO: extend host buffers for vertex/TexCoords/TexID data.
            // scheduler_.vecs.push_back(...)
            // ...
        }
    }

    void destroyAtlas(DestroyAtlas const& _atlas)
    {
        if (auto const it = atlasMap_.find(_atlas.atlas); it != atlasMap_.end())
        {
            GLuint const textureId = it->second;
            atlasMap_.erase(it);
            glDeleteTextures(1, &textureId);
        }
    }

  private:
    void bindTexture2DArray(GLuint _textureId)
    {
        if (currentTextureId_ != _textureId)
        {
            glBindTexture(GL_TEXTURE_2D_ARRAY, _textureId);
            currentTextureId_ = _textureId;
        }
    }

    void setActiveTexture(unsigned _id)
    {
        if (currentActiveTexture_ != _id)
        {
            glActiveTexture(GL_TEXTURE0 + _id);
            currentActiveTexture_ = _id;
        }
    }

  private:
    GLuint vao_;
    GLuint vbo_; // position
    GLuint vboTex_; // TexCoords (vec2) and Texture ID

    ExecutionScheduler scheduler_;

    std::map<unsigned, GLuint> atlasMap_{}; // maps atlas IDs to texture IDs

    GLuint currentActiveTexture_ = 0;
    GLuint currentTextureId_ = 0;
};

} // end namespace
