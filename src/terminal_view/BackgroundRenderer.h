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

namespace terminal::view {

struct ScreenCoordinates;
class OpenGLRenderer;

class BackgroundRenderer {
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param _screenCoordinates
    /// @param _defaultColor
    /// @param _renderTarget
    BackgroundRenderer(ScreenCoordinates const& _screenCoordinates,
                       RGBColor const& _defaultColor,
                       OpenGLRenderer& _renderTarget);

    void setDefaultColor(RGBColor const& _color) noexcept { defaultColor_ = _color; }

    constexpr void setOpacity(float _value) noexcept { opacity_ = _value; }

    // TODO: pass background color directly (instead of whole grid cell),
    // because there is no need to detect bg/fg color more than once per grid cell!

    /// Queues up a render with given background
    void renderCell(Coordinate const& _pos, RGBColor const& _color);

    void renderOnce(Coordinate const& _pos, RGBColor const& _color, unsigned _count);

    void renderPendingCells();
    void finish();

  private:
    void renderCellRange();

  private:
    ScreenCoordinates const& screenCoordinates_;
    RGBColor defaultColor_;
    float opacity_ = 1.0f; // normalized opacity value between 0.0 .. 1.0

    // input state
    RGBColor color_{};
    cursor_pos_t row_ = 0;
    cursor_pos_t startColumn_ = 0;
    unsigned columnCount_ = 0;

    // rendering
    OpenGLRenderer& renderTarget_;
};

} // end namespace
