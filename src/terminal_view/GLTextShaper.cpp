/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal_view/GLTextShaper.h>
#include <terminal_view/FontManager.h>

using namespace std;

namespace terminal::view {

GLTextShaper::Glyph::~Glyph()
{
    // TODO: release texture
}

GLTextShaper::GLTextShaper(Font& _regularFont,
                           QMatrix4x4 const& _projection,
                           ShaderConfig const& _shaderConfig) :
    cache_{},
    regularFont_{ _regularFont },
    //projectionMatrix_{ _projection },
    shader_{},
    colorLocation_{}
{
    initializeOpenGLFunctions();
    if (!setShaderConfig(_shaderConfig))
        throw std::runtime_error("Could not load shaders.");

    // disable byte-alignment restriction
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Configure VAO/VBO for texture quads
    vao_.create();
    vao_.bind();

    vbo_.create();
    vbo_.bind();

    vbo_.setUsagePattern(QOpenGLBuffer::UsagePattern::DynamicDraw);
    vbo_.allocate(sizeof(GLfloat) * 6 * 4);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);

    vbo_.release();
    vao_.release();

    setProjection(_projection);
}

GLTextShaper::~GLTextShaper()
{
    // TODO: release vbo / vba
}

bool GLTextShaper::setShaderConfig(ShaderConfig const& _shaderConfig)
{
    auto shader = createShader(_shaderConfig);
    if (!shader)
        return false;

    colorLocation_ = shader->uniformLocation("textColor");
    projectionLocation_ = shader->uniformLocation("projection");
    shader.swap(shader_);
    return true;
}

void GLTextShaper::setFont(Font& _regularFont)
{
    regularFont_ = _regularFont;
    clearGlyphCache();
}

void GLTextShaper::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    projectionMatrix_ = _projectionMatrix;
}

void GLTextShaper::render(QPoint _pos,
                          std::vector<char32_t> const& _chars,
                          QVector4D const& _color,
                          [[maybe_unused]] FontStyle _style)
{
    Font& font = regularFont_.get(); // TODO: respect _style

    font.render(_chars, glyphPositions_);

    shader_->bind();
    shader_->setUniformValue(colorLocation_, _color);
    shader_->setUniformValue(projectionLocation_, projectionMatrix_);
    glActiveTexture(GL_TEXTURE0);
    vao_.bind();
    vbo_.bind();

    for (Font::GlyphPosition const& gpos : glyphPositions_)
    {
        if (gpos.glyphIndex == 0)
            continue;

        Glyph const& glyph = getGlyphByIndex(gpos.font.get(), gpos.glyphIndex);
        unsigned const x = _pos.x() + gpos.x;
        unsigned const y = _pos.y() + gpos.y;

        auto const xpos = static_cast<GLfloat>(x + glyph.bearing.x());
        auto const ypos = static_cast<GLfloat>(y + font.baseline() - glyph.descender);
        auto const w = static_cast<GLfloat>(glyph.size.x());
        auto const h = static_cast<GLfloat>(glyph.size.y());

        GLfloat const vertices[6][4] = {
            { xpos,     ypos + h,   0.0, 0.0 },
            { xpos,     ypos,       0.0, 1.0 },
            { xpos + w, ypos,       1.0, 1.0 },

            { xpos,     ypos + h,   0.0, 0.0 },
            { xpos + w, ypos,       1.0, 1.0 },
            { xpos + w, ypos + h,   1.0, 0.0 }
        };

        glBindTexture(GL_TEXTURE_2D, glyph.textureID);
        // XXX glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        vbo_.write(0, vertices, sizeof(vertices));

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glyphPositions_.clear();

    #if !defined(NDEBUG)
    vao_.release();
    vbo_.release();
    #endif
}

GLTextShaper::Glyph& GLTextShaper::getGlyphByIndex(Font& _font, unsigned long _index)
{
    auto& cache = cache_[static_cast<size_t>(FontStyle::Regular)]; // TODO: make cache more intelligent to multiple fonts
    if (auto i = cache.find(_index); i != cache.end())
        return i->second;

    _font.loadGlyphByIndex(_index);

    // Generate texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(
        GL_TEXTURE_2D,                  // target
        0,                              // level
        GL_RED,                         // internal format
        _font->glyph->bitmap.width,     // width
        _font->glyph->bitmap.rows,      // height
        0,                              // border (must be set to 0)
        GL_RED,                         // pixel-data format
        GL_UNSIGNED_BYTE,               // pixel-data type
        _font->glyph->bitmap.buffer     // pixel-data pointer
    );

    // Set texture options
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // store character for later use
    auto const descender = _font->glyph->metrics.height / 64 - _font->glyph->bitmap_top;
    Glyph& glyph = cache.emplace(make_pair(_index, Glyph{
        texture,
        QPoint(static_cast<int>(_font->glyph->bitmap.width), static_cast<int>(_font->glyph->bitmap.rows)),
        QPoint(_font->glyph->bitmap_left, _font->glyph->bitmap_top),
        static_cast<unsigned>(_font->height) / 64,
        static_cast<unsigned>(descender),
        static_cast<unsigned>(_font->glyph->advance.x / 64)
    })).first->second;

    return glyph;
}

void GLTextShaper::clearGlyphCache()
{
    for (auto& cache: cache_)
        cache.clear();
}

} // namespace terminal::view
