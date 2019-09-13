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
#include "CellBackground.h"
#include <GL/glew.h>
#include <glm/gtc/matrix_transform.hpp>

CellBackground::CellBackground(unsigned _width, unsigned _height, glm::mat4 const& _projectionMatrix)
{
    transformLocation_ = shader_.uniformLocation("transform");
    projectionMatrix_ = _projectionMatrix;

    // setup background shader
    GLfloat const vertices[] = {
        0.0f, 0.0f,                           // bottom left
        (GLfloat)_width, 0.0f,                // bottom right
        (GLfloat)_width, (GLfloat)_height,    // top right
        0.0f, (GLfloat)_height                // top left
    };

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // specify vertex data layout
    auto posAttr = glGetAttribLocation(shader_, "position");
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

void CellBackground::render(glm::ivec2 _pos, terminal::RGBColor const& color)
{
    shader_.use();
    shader_.setVec3("backgroundColor", glm::vec3{ color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f });

    glm::mat4 const translation = glm::translate(glm::mat4(1.0f), glm::vec3(_pos[0], _pos[1], 0.0f));
    shader_.setMat4(transformLocation_, projectionMatrix_ * translation);

    glBindVertexArray(vao_);
    glDrawArrays(GL_QUADS, 0, 4);
}
