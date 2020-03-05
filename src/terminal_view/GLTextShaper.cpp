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

GLTextShaper::GLTextShaper(Font& _regularFont, QMatrix4x4 const& _projection) :
    cache_{},
    regularFont_{ _regularFont },
    //projectionMatrix_{ _projection },
    shader_{},
    colorLocation_{}
{
    initializeOpenGLFunctions();
    shader_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderCode().c_str());
    shader_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderCode().c_str());
    shader_.link();
    if (!shader_.isLinked())
    {
        qDebug() << "GLCursor: Failed to link shader.";
        qDebug() << "GLTextShaper.shader. " << shader_.log();
    }
    colorLocation_ = shader_.uniformLocation("textColor");
    projectionLocation_ = shader_.uniformLocation("projection");

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

string const& GLTextShaper::vertexShaderCode()
{
    static string const code = R"(
        #version 130
        in vec4 vertex;
        varying vec2 TexCoords;

        uniform mat4 projection;

        void main()
        {
            gl_Position = projection * vec4(vertex.xy, 0.1, 1.0);
            TexCoords = vertex.zw;
        }
    )";
    return code;
}

string const& GLTextShaper::fragmentShaderCode()
{
    static string const code = R"(
        #version 130
        in vec2 TexCoords;
        out vec4 color;

        uniform sampler2D text;
        uniform vec4 textColor;

        void main()
        {
            vec4 sampled = vec4(1.0, 1.0, 1.0, texture2D(text, TexCoords).r);
            color = textColor * sampled;
        }
    )";
    return code;
}

void GLTextShaper::setFont(Font& _regularFont)
{
    regularFont_ = _regularFont;
    clearGlyphCache();
}

void GLTextShaper::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    shader_.bind();
    shader_.setUniformValue(projectionLocation_, _projectionMatrix);
}

void GLTextShaper::render(
    QPoint _pos,
    std::vector<char32_t> const& _chars,
    QVector4D const& _color,
    FontStyle _style)
{
    Font& font = regularFont_.get(); // TODO: respect _style

    font.render(_chars, glyphPositions_);

    shader_.bind();
    shader_.setUniformValue(colorLocation_, _color);
    glActiveTexture(GL_TEXTURE0);
    vao_.bind();
    vbo_.bind();

    for (auto const& gpos : glyphPositions_)
    {
        if (gpos.codepoint == 0)
            continue;

        Glyph const& glyph = getGlyphByIndex(gpos.codepoint, _style);
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

GLTextShaper::Glyph& GLTextShaper::getGlyphByIndex(unsigned long _index, FontStyle _style)
{
    auto& cache = cache_[static_cast<size_t>(_style)];
    if (auto i = cache.find(_index); i != cache.end())
        return i->second;

    Font& font = regularFont_; // TODO: respect _style
    font.loadGlyphByIndex(_index);

    // Generate texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RED,
        font->glyph->bitmap.width,
        font->glyph->bitmap.rows,
        0,
        GL_RED,
        GL_UNSIGNED_BYTE,
        font->glyph->bitmap.buffer
    );

    // Set texture options
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // store character for later use
    auto const descender = font->glyph->metrics.height / 64 - font->glyph->bitmap_top;
    Glyph& glyph = cache.emplace(make_pair(_index, Glyph{
        texture,
        QPoint(static_cast<int>(font->glyph->bitmap.width), static_cast<int>(font->glyph->bitmap.rows)),
        QPoint(font->glyph->bitmap_left, font->glyph->bitmap_top),
        static_cast<unsigned>(font->height) / 64,
        static_cast<unsigned>(descender),
        static_cast<unsigned>(font->glyph->advance.x / 64)
    })).first->second;

    return glyph;
}

void GLTextShaper::clearGlyphCache()
{
    for (auto& cache: cache_)
        cache.clear();
}

} // namespace terminal::view
