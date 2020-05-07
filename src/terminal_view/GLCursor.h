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

#include <terminal/Commands.h>
#include <terminal_view/ShaderConfig.h>

#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLShaderProgram>
#include <QtGui/QVector4D>
#include <QtGui/QOpenGLFunctions>
#include <QtGui/QOpenGLBuffer>
#include <QtGui/QOpenGLVertexArrayObject>

#include <memory>
#include <string>

namespace terminal::view {

class GLCursor : public QOpenGLFunctions {
  public:
    GLCursor(QSize _size,
             QMatrix4x4 _transform,
             CursorShape _shape,
             QVector4D const& _color,
             ShaderConfig const& _shaderConfig);
    ~GLCursor();

    bool setShaderConfig(ShaderConfig const& _shaderConfig);
    void setProjection(QMatrix4x4 const& _mat);

    CursorShape shape() const noexcept { return shape_; }
    void setShape(CursorShape _shape);
    void setColor(QVector4D const& _color);

    void resize(QSize _size);
    void render(QPoint _pos, unsigned _columnWidth);

  private:
    void updateShape();

  private:
    CursorShape shape_;
    QSize size_;
    unsigned columnWidth_;
    QMatrix4x4 projectionMatrix_;
    std::unique_ptr<QOpenGLShaderProgram> shader_;
    GLint transformLocation_;
    GLint colorLocation_;
    QOpenGLBuffer vbo_;
    QOpenGLVertexArrayObject vao_;
    GLenum drawMode_;
    GLsizei drawCount_;
};

} // namespace terminal::view
