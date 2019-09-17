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

#include <glm/gtc/matrix_transform.hpp>

using namespace std;

auto constexpr vertexShader = R"(
    #version 150 core
    in vec2 position;
    uniform mat4 transform;
    void main()
    {
        gl_Position = transform * vec4(position, 0.2, 1.0);
    }
)";

auto constexpr fragmentShader = R"(
    #version 150 core
    uniform vec3 color;
    void main()
    {
        gl_FragColor = vec4(color, 1.0);
    }
)";

GLCursor::GLCursor(glm::ivec2 _size, glm::mat4 _transform, CursorShape _shape, glm::vec3 const& _color) :
    shape_{ _shape },
    transform_{ move(_transform) },
    shader_{ make_unique<Shader>(vertexShader, fragmentShader) },
    transformLocation_{ shader_->uniformLocation("transform") },
    colorLocation_{ shader_->uniformLocation("color") },
    vbo_{},
    vao_{}
{
    setColor(_color);

    // --------------------------------------------------------------------
    // setup vertices
    glGenBuffers(1, &vbo_);
    //TODO: call setSize(_size); instead
    GLfloat const vertices[] = {
        0.0f, 0.0f,                           // bottom left
        (GLfloat)_size.x, 0.0f,               // bottom right
        (GLfloat)_size.x, (GLfloat)_size.y,   // top right
        0.0f, (GLfloat)_size.y                // top left
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // specify vertex data layout
    auto const posAttr = shader_->attributeLocation("position");
    glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(posAttr);
}

GLCursor::~GLCursor()
{
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
}

void GLCursor::setTransform(glm::mat4 _mat)
{
    transform_ = _mat;
}

void GLCursor::setShape(CursorShape _shape)
{
    // TODO: update shaper parameters to reflect new shape (by changing vertices?)
}

void GLCursor::setColor(glm::vec3 _color)
{
    shader_->use();
    shader_->setVec3(colorLocation_, _color);
}

void GLCursor::setSize(glm::vec2 _size)
{
    // TODO: check if this is enough

    GLfloat const vertices[] = {
        0.0f, 0.0f,                           // bottom left
        (GLfloat)_size.x, 0.0f,               // bottom right
        (GLfloat)_size.x, (GLfloat)_size.y,   // top right
        0.0f, (GLfloat)_size.y                // top left
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
}

void GLCursor::render(glm::ivec2 _pos)
{
    // kann alle shapes rendern, same code paths
    shader_->use();

    auto const pos = shader_->uniformLocation("pos");
    shader_->setVec2(pos, glm::vec2{ glm::vec2{_pos.x, _pos.y} });

    glm::mat4 const translation = glm::translate(glm::mat4(1.0f), glm::vec3(_pos[0], _pos[1], 0.0f));
    shader_->setMat4(transformLocation_, transform_ * translation);

    glBindVertexArray(vao_);
    glDrawArrays(GL_QUADS, 0, 4);
}

