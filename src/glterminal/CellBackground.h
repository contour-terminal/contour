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

#include <glterminal/Shader.h>

#include <terminal/Color.h>

#include <GL/glew.h>
#include <glm/glm.hpp>

/// OpenGL Object for rendering character cell's background.
class CellBackground {
  public:
    CellBackground(glm::ivec2 _size, glm::mat4 _projectionMatrix);
    ~CellBackground();

    void setProjection(glm::mat4 const& _projectionMatrix);
    void render(glm::ivec2 _pos, glm::vec4 const& _color);
    void resize(glm::ivec2 _size);

  private:
    glm::mat4 projectionMatrix_;
    Shader shader_;
    GLint const transformLocation_;
    GLint const colorLocation_;
    GLuint vbo_{};
    GLuint vao_{};
};

