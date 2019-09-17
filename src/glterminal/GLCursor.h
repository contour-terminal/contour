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
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <memory>

enum class CursorShape {
    Block,
    Underscore,
    Beam,
};

class GLCursor {
  public:
    GLCursor(glm::ivec2 _size, glm::mat4 _transform,  CursorShape _shape, glm::vec3 const& _color);
    ~GLCursor();

    void setTransform(glm::mat4 _mat); // Must projection matrix.

    CursorShape shape() const noexcept { return shape_; }
    void setShape(CursorShape _shape);

    void setColor(glm::vec3 _color);
    void setSize(glm::vec2 _size);

    void render(glm::ivec2 _pos);

  private:
    CursorShape shape_;
    glm::mat4 transform_;
    std::unique_ptr<Shader> shader_;
    GLint const transformLocation_;
    GLint const colorLocation_;
    GLuint vbo_;
    GLuint vao_;
};
