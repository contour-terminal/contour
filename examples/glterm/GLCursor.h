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
    GLCursor(glm::ivec2 _size, CursorShape _shape);

    void setShape(CursorShape _shape);
    void setTransform(glm::mat4 _mat);
    void resize(glm::ivec2 _size);

    void render(glm::mat4 transform);

  private:
    CursorShape shape_;
    glm::mat4 transform_;
    std::unique_ptr<Shader >shader_;
};
