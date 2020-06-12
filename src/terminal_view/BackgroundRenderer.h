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
#pragma once

#include <terminal_view/ShaderConfig.h>

#include <terminal/Screen.h>

#include <memory>

#include <QtGui/QOpenGLBuffer>
#include <QtGui/QOpenGLShaderProgram>
#include <QtGui/QOpenGLVertexArrayObject>
#include <QtGui/QOpenGLExtraFunctions>

namespace terminal::view {

struct ScreenCoordinates;

class BackgroundRenderer : public QOpenGLExtraFunctions {
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param _screenCoordinates
    /// @param _projectionMatrix
    /// @param _shaderConfig
    BackgroundRenderer(ScreenCoordinates const& _screenCoordinates,
                       ColorProfile const& _colorProfile,
                       QMatrix4x4 const& _projectionMatrix,
                       ShaderConfig const& _shaderConfig);

    ~BackgroundRenderer();

    void setProjection(QMatrix4x4 const& _projectionMatrix);
    void setColorProfile(ColorProfile const& _colorProfile);

    // TODO: pass background color directly (instead of whole grid cell),
    // because there is no need to detect bg/fg color more than once per grid cell!

    /// Queues up a render with given background
    void renderCell(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell);
    void renderCell(cursor_pos_t _row, cursor_pos_t _col, RGBColor const& _color);

    void renderOnce(cursor_pos_t _row, cursor_pos_t _col, RGBColor const& _color, unsigned _count);

    /// Executes all queued render actions
    void execute();

    constexpr void setOpacity(float _value) noexcept { opacity_ = _value; }

  private:
    void renderCellRange();

  private:
    ScreenCoordinates const& screenCoordinates_;
    ColorProfile colorProfile_; // TODO: make const&, maybe reference_wrapper<>?
    float opacity_ = 1.0f; // normalized opacity value between 0.0 .. 1.0

    // input state
    RGBColor color_{};
    cursor_pos_t row_ = 0;
    cursor_pos_t startColumn_ = 0;
    unsigned columnCount_ = 0;

    // scheduled renders
    std::vector<GLfloat> vertexCoords_{};
    std::vector<GLfloat> colors_{};

    // rendering
    QMatrix4x4 projectionMatrix_;
    std::unique_ptr<QOpenGLShaderProgram> shader_;
    GLint projectionLocation_;
    GLuint colorsBuffer_;       // Buffer containing the text colors
    GLuint vbo_;
    GLuint vao_;
};

} // end namespace
