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
#include <terminal_view/OpenGLRenderer.h>

#include <crispy/algorithm.h>

#include <algorithm>
#include <iostream>

namespace terminal::view {

BackgroundRenderer::BackgroundRenderer(ScreenCoordinates const& _screenCoordinates,
                                       ColorProfile const& _colorProfile,
                                       OpenGLRenderer& _renderTarget) :
    screenCoordinates_{ _screenCoordinates },
    colorProfile_{ _colorProfile },
    renderTarget_{ _renderTarget }
{
}

void BackgroundRenderer::renderCell(cursor_pos_t _row, cursor_pos_t _col, Cell const& _cell)
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
        renderPendingCells();

        startColumn_ = _col;
        row_ = _row;
        color_ = _color;
        columnCount_ = 1;
    }
}

void BackgroundRenderer::renderOnce(cursor_pos_t _row, cursor_pos_t _col, RGBColor const& _color, unsigned _count)
{
    renderPendingCells();

    startColumn_ = _col;
    row_ = _row;
    columnCount_ = _count;
    color_ = _color;

    renderCellRange();
}

void BackgroundRenderer::renderCellRange()
{
    if (color_ == colorProfile_.defaultBackground)
        return;

    auto const pos = QPoint{screenCoordinates_.map(startColumn_, row_)};

    auto const color = QVector4D{static_cast<float>(color_.red) / 255.0f,
                                 static_cast<float>(color_.green) / 255.0f,
                                 static_cast<float>(color_.blue) / 255.0f,
                                 opacity_};

    renderTarget_.renderRectangle(
        static_cast<unsigned>(pos.x()),
        static_cast<unsigned>(pos.y()),
        screenCoordinates_.cellWidth * columnCount_,
        screenCoordinates_.cellHeight,
        color
    );

    columnCount_ = 0;
    startColumn_ = 0;
    row_ = 0;
}

void BackgroundRenderer::renderPendingCells()
{
    if (columnCount_)
        renderCellRange();
}

void BackgroundRenderer::finish()
{
    startColumn_ = 0;
    row_ = 0;
    color_ = RGBColor{};
    columnCount_ = 0;
}

} // end namespace
