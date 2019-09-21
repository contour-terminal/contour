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
#include <glterminal/GLCursor.h>

#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>

using namespace std;

CursorShape makeCursorShape(string const& _name)
{
    if (_name == "block")
        return CursorShape::Block;

    if (_name == "underscore")
        return CursorShape::Underscore;

    if (_name == "beam")
        return CursorShape::Beam;

    throw runtime_error("Invalid cursor shape. Use one of block, underscore, beam.");
}

string to_string(CursorShape _value)
{
    switch (_value)
    {
        case CursorShape::Block:
            return "block";
        case CursorShape::Underscore:
            return "underscroe";
        case CursorShape::Beam:
            return "beam";
    }
    return "block";
}

auto constexpr vertexShader = R"(
    #version 140
    in vec2 position;
    uniform mat4 u_transform;
    void main()
    {
        gl_Position = u_transform * vec4(position, 0.2, 1.0);
    }
)";

auto constexpr fragmentShader = R"(
    #version 140
    uniform vec3 u_color;
    void main()
    {
        gl_FragColor = vec4(u_color, 1.0);
    }
)";

GLCursor::GLCursor(glm::ivec2 _size, glm::mat4 _transform, CursorShape _shape, glm::vec3 const& _color) :
    shape_{ _shape },
    projectionMatrix_{ move(_transform) },
    shader_{ vertexShader, fragmentShader },
    transformLocation_{ shader_.uniformLocation("u_transform") },
    colorLocation_{ shader_.uniformLocation("u_color") },
    vbo_{},
    vao_{}
{
    setColor(_color);

    // --------------------------------------------------------------------
    // setup vertices
    //TODO: call setSize(_size); instead
    GLfloat const vertices[] = {
        0.0f, 0.0f,                           // bottom left
        (GLfloat)_size.x, 0.0f,               // bottom right
        (GLfloat)_size.x, (GLfloat)_size.y,   // top right
        0.0f, (GLfloat)_size.y                // top left
    };

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // specify vertex data layout
    auto const posAttr = shader_.attributeLocation("position");
    glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(posAttr);
}

GLCursor::~GLCursor()
{
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
}

void GLCursor::setProjection(glm::mat4 const& _mat)
{
    projectionMatrix_ = _mat;
}

void GLCursor::setShape(CursorShape _shape)
{
    // TODO: update shaper parameters to reflect new shape (by changing vertices?)
}

void GLCursor::setColor(glm::vec3 _color)
{
    shader_.use();
    shader_.setVec3(colorLocation_, _color);
}

void GLCursor::resize(glm::ivec2 _size)
{
    GLfloat const vertices[] = {
        0.0f, 0.0f,                             // bottom left
        (GLfloat) _size.x, 0.0f,                // bottom right
        (GLfloat) _size.x, (GLfloat) _size.y,   // top right
        0.0f, (GLfloat) _size.y                 // top left
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLCursor::render(glm::ivec2 _pos)
{
    shader_.use();

    glm::mat4 const translation = glm::translate(glm::mat4(1.0f), glm::vec3(_pos.x, _pos.y, 0.0f));
    shader_.setMat4(transformLocation_, projectionMatrix_ * translation);

    glBindVertexArray(vao_);
    glDrawArrays(GL_QUADS, 0, 4);
}

