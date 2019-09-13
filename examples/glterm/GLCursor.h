#pragma once

#include "Shader.h"
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
