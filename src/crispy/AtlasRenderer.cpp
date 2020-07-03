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
#include <crispy/AtlasRenderer.h>
#include <crispy/Atlas.h>
#include <crispy/algorithm.h>

#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLTexture>

#include <algorithm>
#include <iostream>

using namespace std;
using namespace std::placeholders;

// TODO: check for GL_OUT_OF_MEMORY in GL allocation/store functions

namespace crispy::atlas {

struct Renderer::ExecutionScheduler : public CommandListener
{
    std::vector<CreateAtlas> createAtlases;
    std::vector<UploadTexture> uploadTextures;
    std::vector<RenderTexture> renderTextures;
    std::vector<GLfloat> vertexCoords;
    std::vector<GLfloat> texCoords;
    std::vector<GLfloat> colors;
    std::vector<DestroyAtlas> destroyAtlases;

    void createAtlas(CreateAtlas const& _atlas) override
    {
        createAtlases.emplace_back(_atlas);
    }

    void uploadTexture(UploadTexture const& _texture) override
    {
        uploadTextures.emplace_back(_texture);
    }

    void renderTexture(RenderTexture const& _render) override
    {
        renderTextures.emplace_back(_render);

        { // vertex coordinates
            GLfloat const x = _render.x;
            GLfloat const y = _render.y;
            GLfloat const z = _render.z;
          //GLfloat const w = _render.w;
            GLfloat const r = _render.texture.get().targetWidth;
            GLfloat const s = _render.texture.get().targetHeight;

            GLfloat const vertices[6 * 3] = {
                // first triangle
                x,     y + s, z,
                x,     y,     z,
                x + r, y,     z,

                // second triangle
                x,     y + s, z,
                x + r, y,     z,
                x + r, y + s, z
            };

            copy(vertices, back_inserter(this->vertexCoords));
            assert(this->vertexCoords.size() == 6 * 3 * renderTextures.size());
        }

        { // texture coordinates
            GLfloat const rx = _render.texture.get().relativeX;
            GLfloat const ry = _render.texture.get().relativeY;
            GLfloat const w = _render.texture.get().relativeWidth;
            GLfloat const h = _render.texture.get().relativeHeight;
            GLfloat const i = _render.texture.get().z;
            GLfloat const u = _render.texture.get().user;
            GLfloat const texCoords[6 * 4] = {
                // first triangle
                rx,      ry,     i, u,
                rx,      ry + h, i, u,
                rx + w,  ry + h, i, u,

                // second triangle
                rx,      ry,     i, u,
                rx + w,  ry + h, i, u,
                rx + w,  ry,     i, u,
            };
            copy(texCoords, back_inserter(this->texCoords));
        }

        // texture color that MAY be blended onto the texture
        for (size_t i = 0; i < 6; ++i)
        {
            colors.push_back(_render.color[0]);
            colors.push_back(_render.color[1]);
            colors.push_back(_render.color[2]);
            colors.push_back(_render.color[3]);
        }
        assert(colors.size() == 6 * 4 * renderTextures.size());
    }

    void destroyAtlas(DestroyAtlas const& _atlas) override
    {
        destroyAtlases.push_back(_atlas);
    }

    size_t size() const noexcept
    {
        return createAtlases.size()
             + uploadTextures.size()
             + renderTextures.size()
             + destroyAtlases.size();
    }

    void reset()
    {
        createAtlases.clear();
        uploadTextures.clear();
        renderTextures.clear();
        vertexCoords.clear();
        texCoords.clear();
        colors.clear();
        destroyAtlases.clear();
    }
};

Renderer::Renderer() :
    scheduler_{std::make_unique<ExecutionScheduler>()}
{
    initializeOpenGLFunctions();

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // 0 (vec3): vertex buffer
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    // 1 (vec3): texture coordinates buffer
    glGenBuffers(1, &texCoordsBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, texCoordsBuffer_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);

    // 2 (vec4): color buffer
    glGenBuffers(1, &colorsBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, colorsBuffer_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(2);
}

Renderer::~Renderer()
{
    for ([[maybe_unused]] auto [_, textureId] : atlasMap_)
        glDeleteTextures(1, &textureId);

    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &vbo_);
    glDeleteBuffers(1, &texCoordsBuffer_);
    glDeleteBuffers(1, &colorsBuffer_);
}

CommandListener& Renderer::scheduler() noexcept
{
    return *scheduler_;
}

unsigned Renderer::maxTextureDepth()
{
    GLint value;
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &value);
    return static_cast<unsigned>(value);
}

unsigned Renderer::maxTextureSize()
{
    GLint value = {};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    return static_cast<unsigned>(value);
}

unsigned Renderer::maxTextureUnits()
{
    GLint value = {};
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &value);
    return static_cast<unsigned>(value);
}

void Renderer::setProjection(QMatrix4x4 const& _projection)
{
    projection_ = _projection;
}

size_t Renderer::size() const noexcept
{
    return scheduler_->size();
}

bool Renderer::empty() const noexcept
{
    return scheduler_->size() == 0;
}

/// Executes all scheduled commands in proper order.
void Renderer::execute()
{
    // potentially create new atlases
    for (CreateAtlas const& params : scheduler_->createAtlases)
        createAtlas(params);

    // potentially upload any new textures
    for (UploadTexture const& params : scheduler_->uploadTextures)
        uploadTexture(params);

    // order and prepare texture geometry
    sort(scheduler_->renderTextures.begin(),
         scheduler_->renderTextures.end(),
         [](RenderTexture const& a, RenderTexture const& b) { return a.texture.get().atlas < b.texture.get().atlas; });

    for (RenderTexture const& params : scheduler_->renderTextures)
        renderTexture(params);

    // upload vertices and render (iff there is anything to render)
    if (!scheduler_->renderTextures.empty())
    {
        glBindVertexArray(vao_);

        // upload vertices
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     scheduler_->vertexCoords.size() * sizeof(GLfloat),
                     scheduler_->vertexCoords.data(),
                     GL_STATIC_DRAW);

        // upload texture coordinates
        glBindBuffer(GL_ARRAY_BUFFER, texCoordsBuffer_);
        glBufferData(GL_ARRAY_BUFFER,
                     scheduler_->texCoords.size() * sizeof(GLfloat),
                     scheduler_->texCoords.data(),
                     GL_STATIC_DRAW);

        // upload text colors
        glBindBuffer(GL_ARRAY_BUFFER, colorsBuffer_);
        glBufferData(GL_ARRAY_BUFFER,
                     scheduler_->colors.size() * sizeof(GLfloat),
                     scheduler_->colors.data(),
                     GL_STATIC_DRAW);

        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(scheduler_->vertexCoords.size()));

        // TODO: Instead of on glDrawArrays (and many if's in the shader for each GL_TEXTUREi),
        //       make a loop over each GL_TEXTUREi and draw a sub range of the vertices and a
        //       fixed GL_TEXTURE0. - will this be noticable faster?
    }

    // destroy any pending atlases that were meant to be destroyed
    for (DestroyAtlas const& params : scheduler_->destroyAtlases)
        destroyAtlas(params);

    // reset execution state
    scheduler_->reset();
    currentActiveTexture_ = std::numeric_limits<GLuint>::max();
    currentTextureId_ = std::numeric_limits<GLuint>::max();
}

void Renderer::createAtlas(CreateAtlas const& _atlas)
{
    GLuint textureId{};
    glGenTextures(1, &textureId);
    bindTexture2DArray(textureId);

    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, _atlas.format, _atlas.width, _atlas.height, _atlas.depth);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    auto const key = AtlasKey{_atlas.atlasName, _atlas.atlas};
    atlasMap_[key] = textureId;
}

void Renderer::uploadTexture(UploadTexture const& _upload)
{
    auto const& texture = _upload.texture.get();
    auto const key = AtlasKey{_upload.texture.get().atlasName, _upload.texture.get().atlas};
    [[maybe_unused]] auto const textureIdIter = atlasMap_.find(key);
    assert(textureIdIter != atlasMap_.end() && "Texture ID not found in atlas map!");;
    auto const textureId = atlasMap_[key];
    auto const x0 = texture.x;
    auto const y0 = texture.y;
    auto const z0 = texture.z;

    //cout << "Renderer.uploadTexture(tid:" << textureId << "): " << _upload << endl;

    auto constexpr target = GL_TEXTURE_2D_ARRAY;
    auto constexpr levelOfDetail = 0;
    auto constexpr depth = 1;
    auto constexpr type = GL_UNSIGNED_BYTE;

    bindTexture2DArray(textureId);

    glTexSubImage3D(target, levelOfDetail, x0, y0, z0, texture.width, texture.height, depth,
                    _upload.format, type, _upload.data.data());
}

void Renderer::renderTexture(RenderTexture const& _render)
{
    auto const key = AtlasKey{_render.texture.get().atlasName, _render.texture.get().atlas};
    if (auto const it = atlasMap_.find(key); it != atlasMap_.end())
    {
        GLuint const textureUnit = _render.texture.get().atlas;
        GLuint const textureId = it->second;

        //cout << "Renderer.renderTexture(" << textureUnit << '/' << textureId << "): " << _render << endl;

        selectTextureUnit(textureUnit);
        bindTexture2DArray(textureId);
    }
}

void Renderer::destroyAtlas(DestroyAtlas const& _atlas)
{
    auto const key = AtlasKey{_atlas.atlasName.get(), _atlas.atlas};
    if (auto const it = atlasMap_.find(key); it != atlasMap_.end())
    {
        GLuint const textureId = it->second;
        atlasMap_.erase(it);
        glDeleteTextures(1, &textureId);
    }
}

void Renderer::bindTexture2DArray(GLuint _textureId)
{
    if (currentTextureId_ != _textureId)
    {
        glBindTexture(GL_TEXTURE_2D_ARRAY, _textureId);
        currentTextureId_ = _textureId;
    }
}

void Renderer::selectTextureUnit(unsigned _id)
{
    if (currentActiveTexture_ != _id)
    {
        glActiveTexture(GL_TEXTURE0 + _id);
        currentActiveTexture_ = _id;
    }
}

} // end namespace
