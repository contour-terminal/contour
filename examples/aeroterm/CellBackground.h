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

#include "Shader.h"

#include <terminal/Color.h>

#include <GL/glew.h>
#include <glm/glm.hpp>

/// OpenGL Object for rendering character cell's background.
class CellBackground {
  public:
    CellBackground(unsigned _width, unsigned _height, glm::mat4 const& _projectionMatrix);
    ~CellBackground();

    void setProjection(glm::mat4 const& _projectionMatrix);
    void render(glm::ivec2 _translation, glm::vec4 const& _color);

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
                gl_Position = transform * vec4(position, 0.0, 1.0);
            }
        )";
    }

    static std::string fragmentShader()
    {
        return R"(
            // Fragment Shader
            #version 150 core
            out vec4 outColor;
            uniform vec4 backgroundColor;
            void main()
            {
                outColor = backgroundColor;
            }
        )";
    }

    Shader shader_{ vertexShader(), fragmentShader() };
    GLuint vbo_{};
    GLuint vao_{};

    GLint transformLocation_;
    glm::mat4 projectionMatrix_;
};

