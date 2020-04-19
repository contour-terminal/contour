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
#pragma once

#include <terminal_view/FontManager.h>
#include <terminal_view/ShaderConfig.h>

#include <QMatrix4x4>
#include <QPoint>
#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions>
#include <QtGui/QOpenGLBuffer>
#include <QtGui/QOpenGLVertexArrayObject>

#include <array>
#include <functional>
#include <unordered_map>
#include <vector>

namespace terminal::view {

class GLTextShaper : private QOpenGLFunctions {
  public:
    GLTextShaper(Font& _regularFont,
                 QMatrix4x4 const& _projection,
                 ShaderConfig const& _shaderConfig);
    ~GLTextShaper();

    void setFont(Font& _regularFont);
    bool setShaderConfig(ShaderConfig const& _shaderConfig);
    void setProjection(QMatrix4x4 const& _projectionMatrix);

    void render(
        QPoint _pos,
        std::vector<char32_t> const& _chars,
        QVector4D const& _color,
        FontStyle _style);

    void clearGlyphCache();

  private:
    struct Glyph {
        GLuint textureID;
        QPoint size;      // glyph size
        QPoint bearing;   // offset from baseline to left/top of glyph
        unsigned height;
        unsigned descender;
        unsigned advance;     // offset to advance to next glyph in line.

        ~Glyph();
    };

    Glyph& getGlyphByIndex(Font& _font, unsigned long _index);

  private:
    std::array<std::unordered_map<unsigned /*glyph index*/, Glyph>, 4> cache_;
    std::reference_wrapper<Font> regularFont_;
    std::vector<Font::GlyphPosition> glyphPositions_;
    QOpenGLBuffer vbo_;
    QOpenGLVertexArrayObject vao_;
    QMatrix4x4 projectionMatrix_;
    std::unique_ptr<QOpenGLShaderProgram> shader_;
    GLint colorLocation_;
    GLuint projectionLocation_;
};

} // namespace terminal::view
