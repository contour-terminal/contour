#include "CellBackground.h"

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

CellBackground::~CellBackground()
{
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
}

void CellBackground::onResize(unsigned _width, unsigned _height)
{
    projectionMatrix_ = glm::ortho(0.0f, static_cast<GLfloat>(_width), 0.0f, static_cast<GLfloat>(_height));
}

void CellBackground::render(glm::ivec2 _pos, terminal::RGBColor const& color)
{
    shader_.use();
    shader_.setVec3("backgroundColor", glm::vec3{ color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f });

    glm::mat4 const translation = glm::translate(glm::mat4(1.0f), glm::vec3(_pos[0], _pos[1], 0.0f));
    shader_.setMat4("transform", projectionMatrix_ * translation);

    glBindVertexArray(vao_);
    glDrawArrays(GL_QUADS, 0, 4);
}
