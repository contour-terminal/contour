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
#include <terminal_view/GLCursor.h>

#include <stdexcept>
#include <vector>

using namespace std;

namespace terminal::view {

CursorShape makeCursorShape(string const& _name)
{
    auto static const toLower = [](string const& _value) -> string {
        string result;
        result.reserve(_value.size());
        transform(begin(_value), end(_value), back_inserter(result), [](auto ch) { return tolower(ch); });
        return result;
    };

    auto const name = toLower(_name);

    if (name == "block")
        return CursorShape::Block;

    if (name == "rectangle")
        return CursorShape::Rectangle;

    if (name == "underscore")
        return CursorShape::Underscore;

    if (name == "bar")
        return CursorShape::Bar;

    throw runtime_error("Invalid cursor shape. Use one of block, underscore, beam.");
}

string to_string(CursorShape _value)
{
    switch (_value)
    {
        case CursorShape::Block:
            return "block";
        case CursorShape::Rectangle:
            return "rectangle";
        case CursorShape::Underscore:
            return "underscroe";
        case CursorShape::Bar:
            return "bar";
    }
    return "block";
}

auto constexpr vertexShader = R"(
    #version 130
    in vec2 position;
    uniform mat4 u_transform;
    void main()
    {
        gl_Position = u_transform * vec4(position, 0.2, 1.0);
    }
)";

auto constexpr fragmentShader = R"(
    #version 130
    uniform vec4 u_color;
    out vec4 outColor;
    void main()
    {
        outColor = u_color;
    }
)";


pair<GLenum, vector<float>> getTriangles(QSize _size, CursorShape _shape)
{
    switch (_shape)
    {
        case CursorShape::Block:
            return {GL_TRIANGLES, vector{
                0.0f, 0.0f,                                         // bottom left
                (GLfloat) _size.width(), 0.0f,                      // bottom right
                (GLfloat) _size.width(), (GLfloat) _size.height(),  // top right

                (GLfloat) _size.width(), (GLfloat) _size.height(),  // top right
                0.0f, (GLfloat) _size.height(),                     // top left
                0.0f, 0.0f                                          // bottom left
            }};
        case CursorShape::Rectangle:
            return {GL_LINE_STRIP, {
                0.0f, 0.0f,                                         // bottom left
                (GLfloat) _size.width(), 0.0f,                      // bottom right

                (GLfloat) _size.width(), (GLfloat) _size.height(),  // top right
                0.0f, (GLfloat) _size.height(),                     // top left

                0.0f, 0.0f                                          // bottom left
            }};
        case CursorShape::Underscore:
            return {GL_LINES, vector{
                0.0f, 0.0f,                                         // bottom left
                (GLfloat) _size.width(), 0.0f,                      // bottom right
            }};
        case CursorShape::Bar:
            return {GL_LINES, vector{
                0.0f, 0.0f,                                         // bottom left
                0.0f, (GLfloat) _size.height(),                     // top left
            }};
            break;
    }
    assert(!"Should not have reached here.");
    return {GL_TRIANGLES, {}};
}

GLCursor::GLCursor(QSize _size, QMatrix4x4 _transform, CursorShape _shape, QVector4D const& _color) :
    shape_{ _shape },
    size_{ _size },
    projectionMatrix_{ _transform },
    shader_{},
    transformLocation_{},
    colorLocation_{},
    vbo_{},
    vao_{}
{
    initializeOpenGLFunctions();
    shader_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    shader_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    shader_.link();
    if (!shader_.isLinked())
        qDebug() << "GLCursor: Failed to link shader.";

    transformLocation_ = shader_.uniformLocation("u_transform");
    colorLocation_ = shader_.uniformLocation("u_color");
    shader_.bind();
    setColor(_color);

    // --------------------------------------------------------------------
    // setup vertices
    vbo_.create();
    vbo_.bind();
    vbo_.setUsagePattern(QOpenGLBuffer::UsagePattern::StaticDraw);
    vbo_.allocate(12 * sizeof(GLfloat));

    vao_.create();
    vao_.bind();

    // specify vertex data layout
    auto const posAttr = shader_.attributeLocation("position");
    glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(posAttr);

    updateShape();
}

GLCursor::~GLCursor()
{
    vbo_.destroy();
    vao_.destroy();
}

void GLCursor::setProjection(QMatrix4x4 const& _mat)
{
    projectionMatrix_ = _mat;
}

void GLCursor::setShape(CursorShape _shape)
{
    if (_shape != shape_)
    {
        shape_ = _shape;
        updateShape();
    }
}

void GLCursor::setColor(QVector4D const& _color)
{
    shader_.bind();
    shader_.setUniformValue(colorLocation_, _color);
}

void GLCursor::resize(QSize _size)
{
    size_ = _size;
    updateShape();
}

void GLCursor::updateShape()
{
    auto [drawMode, vertices] = getTriangles(size_, shape_);
    drawMode_ = drawMode;
    drawCount_ = static_cast<GLsizei>(vertices.size() / 2); // vertex count = array element size divided by dimension (2)

    vbo_.bind();
    vbo_.write(0, &vertices[0], vertices.size() * sizeof(GLfloat));
    vbo_.release();
}

void GLCursor::render(QPoint _pos)
{
    shader_.bind();

    auto translation = QMatrix4x4{};
    translation.translate(static_cast<float>(_pos.x()), static_cast<float>(_pos.y()), 0.0f);

    shader_.setUniformValue(transformLocation_, projectionMatrix_ * translation);

    vao_.bind();
    glDrawArrays(drawMode_, 0, drawCount_);
}

} // namespace terminal::view
