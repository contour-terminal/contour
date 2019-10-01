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
#include <glterminal/CellBackground.h>
#include <GL/glew.h>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>

using namespace std;

auto constexpr vertexShader = R"(
    // Vertex Shader
    #version 140
    in vec2 position;
    uniform mat4 u_transform;
    void main()
    {
        gl_Position = u_transform * vec4(position, 0.0, 1.0);
    }
)";

auto constexpr fragmentShader = R"(
    // Fragment Shader
    #version 140
    uniform vec4 u_color;
    out vec4 outColor;
    void main()
    {
        outColor = u_color;
    }
)";

CellBackground::CellBackground(glm::ivec2 _size, glm::mat4 _projectionMatrix) :
    projectionMatrix_{ move(_projectionMatrix) },
    shader_{ vertexShader, fragmentShader },
    transformLocation_{ shader_.uniformLocation("u_transform") },
    colorLocation_{ shader_.uniformLocation("u_color") }
{
    // setup background shader
    GLfloat const vertices[] = {
        0.0f, 0.0f,                             // bottom left
        (GLfloat) _size.x, 0.0f,                // bottom right
        (GLfloat) _size.x, (GLfloat) _size.y,   // top right

        (GLfloat) _size.x, (GLfloat) _size.y,   // top right
        0.0f, (GLfloat) _size.y,                // top left
        0.0f, 0.0f                              // bottom left
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

void CellBackground::setProjection(glm::mat4 const& _projectionMatrix)
{
    projectionMatrix_ = _projectionMatrix;
}

void CellBackground::resize(glm::ivec2 _size)
{
    GLfloat const vertices[] = {
        0.0f, 0.0f,                             // bottom left
        (GLfloat) _size.x, 0.0f,                // bottom right
        (GLfloat) _size.x, (GLfloat) _size.y,   // top right

        (GLfloat) _size.x, (GLfloat) _size.y,   // top right
        0.0f, (GLfloat) _size.y,                // top left
        0.0f, 0.0f                              // bottom left
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void CellBackground::render(glm::ivec2 _pos, glm::vec4 const& _color)
{
    shader_.use();

    glm::mat4 const translation = glm::translate(glm::mat4(1.0f), glm::vec3(_pos.x, _pos.y, 0.0f));
    shader_.setMat4(transformLocation_, projectionMatrix_ * translation);
    shader_.setVec4(colorLocation_, _color);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
