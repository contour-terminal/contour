/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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
#include <terminal_view/BackgroundRenderer.h>
#include <terminal_view/ScreenCoordinates.h>

#include <crispy/algorithm.h>

#include <algorithm>
#include <iostream>

namespace terminal::view {

BackgroundRenderer::BackgroundRenderer(ScreenCoordinates const& _screenCoordinates,
                                       ColorProfile const& _colorProfile,
                                       QMatrix4x4 const& _projectionMatrix,
                                       ShaderConfig const& _shaderConfig) :
    screenCoordinates_{ _screenCoordinates },
    colorProfile_{ _colorProfile },
    projectionMatrix_{ _projectionMatrix },
    shader_{ createShader(_shaderConfig) },
    projectionLocation_{ shader_->uniformLocation("u_projection") }
{
    initializeOpenGLFunctions();

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // 0 (vec3): vertex buffer
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    // 1 (vec4): color buffer
    glGenBuffers(1, &colorsBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, colorsBuffer_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
}

BackgroundRenderer::~BackgroundRenderer()
{
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &vbo_);
    glDeleteBuffers(1, &colorsBuffer_);
}

void BackgroundRenderer::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    projectionMatrix_ = _projectionMatrix;
}

void BackgroundRenderer::renderCell(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell)
{
    RGBColor const color = _cell.attributes().makeColors(colorProfile_).second;
    renderCell(_row, _col, color);
}

void BackgroundRenderer::renderCell(cursor_pos_t _row, cursor_pos_t _col, RGBColor const& _color)
{
    if (row_ == _row && color_ == _color)
        columnCount_++;
    else
    {
        if (columnCount_)
            renderCellRange();

        startColumn_ = _col;
        row_ = _row;
        color_ = _color;
        columnCount_ = 1;
    }
}

void BackgroundRenderer::renderOnce(cursor_pos_t _row, cursor_pos_t _col, RGBColor const& _color, unsigned _count)
{
    if (columnCount_)
        renderCellRange();

    startColumn_ = _col;
    row_ = _row;
    columnCount_ = _count;
    color_ = _color;

    renderCellRange();
    columnCount_ = 0;
    startColumn_ = 0;
    row_ = 0;
}

void BackgroundRenderer::renderCellRange()
{
    if (color_ == colorProfile_.defaultBackground)
        return;

    //std::cout << fmt::format("BackgroundRenderer.renderCellRange: {}:{}..{} {}\n", row_, startColumn_, columnCount_, color_);

    // enqueue vertex coordinates
    {
        QPoint const pos = screenCoordinates_.map(startColumn_, row_);
        GLfloat const x = pos.x();
        GLfloat const y = pos.y();
        GLfloat const z = 0.0f;
        GLfloat const r = screenCoordinates_.cellWidth * columnCount_;
        GLfloat const s = screenCoordinates_.cellHeight;

        GLfloat const vertices[6 * 3] = {
            // first triangle
            x,     y + s, z,
            x,     y,     z,
            x + r, y,     z,

            // second triangle
            x,     y + s, z,
            x + r, y,     z,
            x + r, y + s, z
        };

        crispy::copy(vertices, back_inserter(vertexCoords_));
    }

    QVector4D const qcolor(static_cast<float>(color_.red) / 255.0f,
                           static_cast<float>(color_.green) / 255.0f,
                           static_cast<float>(color_.blue) / 255.0f,
                           opacity_);

    // texture color that MAY be blended onto the texture
    for (size_t i = 0; i < 6; ++i)
    {
        colors_.push_back(qcolor[0]);
        colors_.push_back(qcolor[1]);
        colors_.push_back(qcolor[2]);
        colors_.push_back(qcolor[3]);
    }
}

void BackgroundRenderer::execute()
{
    if (vertexCoords_.empty())
        return;

    //std::cout << fmt::format("BackgroundRenderer.execute: {}\n", vertexCoords_.size() / 6);

    shader_->bind();
    shader_->setUniformValue(projectionLocation_, projectionMatrix_);

    glBindVertexArray(vao_);

    // upload vertices
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 vertexCoords_.size() * sizeof(GLfloat),
                 vertexCoords_.data(),
                 GL_STATIC_DRAW);

    // upload text colors
    glBindBuffer(GL_ARRAY_BUFFER, colorsBuffer_);
    glBufferData(GL_ARRAY_BUFFER,
                 colors_.size() * sizeof(GLfloat),
                 colors_.data(),
                 GL_STATIC_DRAW);

    // render
    glDrawArrays(GL_TRIANGLES, 0, vertexCoords_.size());

    // cleanup
    shader_->release();
    glBindVertexArray(0);

    colors_.clear();
    vertexCoords_.clear();

    startColumn_ = 0;
    row_ = 0;
    color_ = RGBColor{};
    columnCount_ = 0;
}

} // end namespace
