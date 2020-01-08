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
#include <terminal_view/CellBackground.h>

#include <QVector4D>
#include <QMatrix4x4>
#include <QOpenGLShader>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>

#include <utility>

using namespace std;

namespace terminal::view {

auto constexpr vertexShader = R"(
    // Vertex Shader
    in vec2 position;
    uniform mat4 u_transform;
    void main()
    {
        gl_Position = u_transform * vec4(position, 0.0, 1.0);
    }
)";

auto constexpr fragmentShader = R"(
    // Fragment Shader
    uniform vec4 u_color;
    varying out vec4 outColor;
    void main()
    {
        outColor = u_color;
    }
)";

CellBackground::CellBackground(QSize _size, QMatrix4x4 _projectionMatrix) :
    projectionMatrix_{ move(_projectionMatrix) },
    shader_{},
    transformLocation_{},
    colorLocation_{}
{
    initializeOpenGLFunctions();
    shader_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    shader_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    shader_.link();
    if (!shader_.isLinked())
        qDebug() << "CellBackground: Failed to link shader.";

    transformLocation_ = shader_.uniformLocation("u_transform");
    colorLocation_ = shader_.uniformLocation("u_color");

    // setup background shader
    GLfloat const vertices[] = {
        0.0f, 0.0f,                                             // bottom left
        (GLfloat) _size.width(), 0.0f,                          // bottom right
        (GLfloat) _size.width(), (GLfloat) _size.height(),      // top right

        (GLfloat) _size.width(), (GLfloat) _size.height(),      // top right
        0.0f, (GLfloat) _size.height(),                         // top left
        0.0f, 0.0f                                              // bottom left
    };

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // specify vertex data layout
    auto posAttr = shader_.attributeLocation("position");
    glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(posAttr);
}

CellBackground::~CellBackground()
{
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
}

void CellBackground::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    projectionMatrix_ = _projectionMatrix;
}

void CellBackground::resize(QSize _size)
{
    GLfloat const vertices[] = {
        0.0f, 0.0f,                                         // bottom left
        (GLfloat) _size.width(), 0.0f,                      // bottom right
        (GLfloat) _size.width(), (GLfloat) _size.height(),  // top right

        (GLfloat) _size.width(), (GLfloat) _size.height(),  // top right
        0.0f, (GLfloat) _size.height(),                     // top left
        0.0f, 0.0f                                          // bottom left
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void CellBackground::render(QPoint _pos, QVector4D const& _color)
{
    shader_.bind();

    auto translation = QMatrix4x4{};
    translation.translate(_pos.x(), _pos.y(), 0.0f);
    shader_.setUniformValue(transformLocation_, projectionMatrix_ * translation);
    shader_.setUniformValue(colorLocation_, _color);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

} // namespace terminal::view
