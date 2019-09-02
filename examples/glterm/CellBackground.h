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

#include <GL/glew.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "TextShaper.h"

class CellBackground {
public:
    CellBackground(unsigned _width, unsigned _height);
    ~CellBackground();

    void onResize(unsigned _width, unsigned _height);
    void render(glm::ivec2 pos, terminal::RGBColor const& _color);

private:
    static std::string vertexShader()
    {
        return R"(
            // Vertex Shader
            #version 150 core
            in vec2 position;
            uniform mat4 transform;
            void main()
            {
                gl_Position = transform * vec4(position, -0.5, 1.0);
            }
        )";
    }

    static std::string fragmentShader()
    {
        return R"(
            // Fragment Shader
            #version 150 core
            out vec4 outColor;
            uniform vec3 backgroundColor;
            void main()
            {
                outColor = vec4(backgroundColor, 1.0);
            }
        )";
    }

    Shader shader_{ vertexShader(), fragmentShader() };
    GLuint vbo_{};
    GLuint vao_{};

    glm::mat4 projectionMatrix_;
};

CellBackground::CellBackground(unsigned _width, unsigned _height)
{
    projectionMatrix_ = glm::ortho(0.0f, static_cast<GLfloat>(_width), 0.0f, static_cast<GLfloat>(_height));

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

inline CellBackground::~CellBackground()
{
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
}

inline void CellBackground::onResize(unsigned _width, unsigned _height)
{
    projectionMatrix_ = glm::ortho(0.0f, static_cast<GLfloat>(_width), 0.0f, static_cast<GLfloat>(_height));
}

inline void CellBackground::render(glm::ivec2 _pos, terminal::RGBColor const& color)
{
    shader_.use();
    shader_.setVec3("backgroundColor", glm::vec3{ color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f });

    glm::mat4 const translation = glm::translate(glm::mat4(1.0f), glm::vec3(_pos[0], _pos[1], 0.0f));
    shader_.setMat4("transform", projectionMatrix_ * translation);

    glBindVertexArray(vao_);
    glDrawArrays(GL_QUADS, 0, 4);
}
