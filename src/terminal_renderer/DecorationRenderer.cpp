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
#include <terminal_renderer/DecorationRenderer.h>
#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/Atlas.h>
#include <terminal_renderer/Pixmap.h>

#include <crispy/times.h>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

using crispy::Size;

using std::array;
using std::ceil;
using std::clamp;
using std::floor;
using std::get;
using std::max;
using std::min;
using std::nullopt;
using std::optional;
using std::pair;
using std::string;

namespace terminal::renderer {

optional<Decorator> to_decorator(std::string const& _value)
{
    auto constexpr mappings = array{
        pair{"underline", Decorator::Underline},
        pair{"dotted-underline", Decorator::DottedUnderline},
        pair{"double-underline", Decorator::DoubleUnderline},
        pair{"curly-underline", Decorator::CurlyUnderline},
        pair{"dashed-underline", Decorator::DashedUnderline},
        pair{"overline", Decorator::Overline},
        pair{"crossed-out", Decorator::CrossedOut},
        pair{"framed", Decorator::Framed},
        pair{"encircle", Decorator::Encircle},
    };

    for (auto const& mapping : mappings)
        if (mapping.first == _value)
            return {mapping.second};

    return nullopt;
}

DecorationRenderer::DecorationRenderer(GridMetrics const& _gridMetrics,
                                       Decorator _hyperlinkNormal,
                                       Decorator _hyperlinkHover) :
    gridMetrics_{ _gridMetrics },
    hyperlinkNormal_{ _hyperlinkNormal },
    hyperlinkHover_{ _hyperlinkHover }
{
}

void DecorationRenderer::setRenderTarget(RenderTarget& _renderTarget)
{
    Renderable::setRenderTarget(_renderTarget);
    clearCache();
}

void DecorationRenderer::clearCache()
{
    atlas_ = std::make_unique<Atlas>(monochromeAtlasAllocator());
}

namespace
{
    constexpr bool pointVisibleInCircle(int x, int y, int r)
    {
        return x*x + y*y <= r*r;
        // return -1.0f <= x && x <= +1.0f
        //     && -1.0f <= y && y <= +1.0f;
    }

    constexpr double normalize(int _actual, int _max)
    {
        return double(_actual) / double(_max);
    }
}

void DecorationRenderer::rebuild()
{
    auto const width = gridMetrics_.cellSize.width;

    { // {{{ underline
        auto const thickness_half = max(1, int(ceil(underlineThickness() / 2.0)));
        auto const thickness = thickness_half * 2;
        auto const y0 = max(0, underlinePosition() - thickness_half);
        auto const height = Height(y0 + thickness);
        auto image = atlas::Buffer(*width * *height, 0);

        for (int y = 1; y <= thickness; ++y)
            for (int x = 0; x < *width; ++x)
                image[(*height - y0 - y) * *width + x] = 0xFF;

        atlas_->insert(
            Decorator::Underline,
            ImageSize{width, height},
            ImageSize{width, height},
            move(image)
        );
    } // }}}
    { // {{{ double underline
        auto const thickness_half = max(1, int(ceil(underlineThickness() / 3.0)));
        auto const thickness = max(1, int(ceil(double(underlineThickness()) * 2.0) / 3.0));
        auto const y1 = max(0, underlinePosition() + thickness);
        auto const y0 = max(0, y1 - 3 * thickness);
        auto const height = Height(y1 + thickness);
        auto image = atlas::Buffer(*width * *height, 0);

        for (int y = 1; y <= thickness; ++y)
        {
            for (int x = 0; x < *width; ++x)
            {
                image[(*height - y1 - y) * *width + x] = 0xFF; // top line
                image[(*height - y0 - y) * *width + x] = 0xFF; // bottom line
            }
        }

        atlas_->insert(
            Decorator::DoubleUnderline,
            ImageSize{width, height},
            ImageSize{width, height},
            move(image)
        );
    } // }}}
    { // {{{ curly underline
        auto const height = Height::cast_from(gridMetrics_.underline.position);
        auto const h2 = max(unbox<int>(height) / 2, 1);
        auto const yScalar = h2 - 1;
        auto const xScalar = 2 * M_PI / *width;
        auto const yBase = h2;
        auto block = blockElement(ImageSize{Width(width), height});

        for (int x = 0; x < *width; ++x)
        {
            // Using Wu's antialiasing algorithm to paint the curved line.
            // See: https://www-users.mat.umk.pl//~gruby/teaching/lgim/1_wu.pdf
            auto const y = yScalar * cos(xScalar * x);
            auto const y1 = static_cast<int>(floor(y));
            auto const y2 = static_cast<int>(ceil(y));
            auto const intensity = static_cast<int>(255 * fabs(y - y1));
            block.paintOver(x, yBase + y1, 255 - intensity);
            block.paintOver(x, yBase + y2, intensity);
        }

        atlas_->insert(
            Decorator::CurlyUnderline,
            block.downsampledSize(),
            block.downsampledSize(),
            block.take()
        );
    } // }}}
    { // {{{ dotted underline (use square dots)
        auto const dotHeight = gridMetrics_.underline.thickness;
        auto const dotWidth = dotHeight;
        auto const height = Height(gridMetrics_.underline.position + dotHeight);
        auto const y0 = gridMetrics_.underline.position - dotHeight;
        auto const x0 = 0;
        auto const x1 = unbox<int>(width) / 2;
        auto block = blockElement(ImageSize{width, height});

        for (int y = 0; y < dotHeight; ++y)
        {
            for (int x = 0; x < dotWidth; ++x)
            {
                block.paint(x + x0, y + y0);
                block.paint(x + x1, y + y0);
            }
        }

        atlas_->insert(
            Decorator::DottedUnderline,
            block.downsampledSize(),
            block.downsampledSize(),
            block.take()
        );
    } // }}}
    { // {{{ dashed underline
        // Devides a grid cell's underline in three sub-ranges and only renders first and third one,
        // whereas the middle one is being skipped.
        auto const thickness_half = max(1, int(ceil(underlineThickness() / 2.0)));
        auto const thickness = max(1, thickness_half * 2);
        auto const y0 = max(0, underlinePosition() - thickness_half);
        auto const height = Height(y0 + thickness);
        auto image = atlas::Buffer(*width * *height, 0);

        for (int y = 1; y <= thickness; ++y)
            for (int x = 0; x < *width; ++x)
                if (fabsf(float(x) / float(*width) - 0.5f) >= 0.25f)
                    image[(*height - y0 - y) * *width + x] = 0xFF;

        atlas_->insert(
            Decorator::DashedUnderline,
            ImageSize{width, height},
            ImageSize{width, height},
            move(image)
        );
    } // }}}
    { // {{{ framed
        auto const cellHeight = gridMetrics_.cellSize.height;
        auto const thickness = max(1, underlineThickness() / 2);
        auto image = atlas::Buffer(*width * *cellHeight, 0u);
        auto const gap = 0; // thickness;

        // Draws the top and bottom horizontal lines
        for (int y = gap; y < thickness + gap; ++y)
            for (int x = gap; x < *width - gap; ++x)
            {
                image[y * *width + x] = 0xFF;
                image[(*cellHeight - 1 - y) * *width + x] = 0xFF;
            }

        // Draws the left and right vertical lines
        for (int y = gap; y < *cellHeight - gap; y++)
            for (int x = gap; x < thickness + gap; ++x)
            {
                image[y * *width + x] = 0xFF;
                image[y * *width + (*width - 1 - x)] = 0xFF;
            }

        atlas_->insert(
            Decorator::Framed,
            ImageSize{width, cellHeight},
            ImageSize{width, cellHeight},
            move(image)
        );
    } // }}}
    { // {{{ overline
        auto const cellHeight = gridMetrics_.cellSize.height;
        auto const thickness = underlineThickness();
        auto image = atlas::Buffer(*width * *cellHeight, 0);

        for (int y = 0; y < thickness; ++y)
            for (int x = 0; x < *width; ++x)
                image[(*cellHeight - y - 1) * *width + x] = 0xFF;

        atlas_->insert(
            Decorator::Overline,
            ImageSize{width, cellHeight},
            ImageSize{width, cellHeight},
            move(image)
        );
    } // }}}
    { // {{{ crossed-out
        auto const height = Height(*gridMetrics_.cellSize.height / 2);
        auto const thickness = underlineThickness();
        auto image = atlas::Buffer(*width * *height, 0u);

        for (int y = 1; y <= thickness; ++y)
            for (int x = 0; x < *width; ++x)
                image[(*height - y) * *width + x] = 0xFF;

        atlas_->insert(
            Decorator::CrossedOut,
            ImageSize{width, height},
            ImageSize{width, height},
            move(image)
        );
    } // }}}
    // TODO: Encircle
}

void DecorationRenderer::renderCell(RenderCell const& _cell)
{
    auto constexpr mappings = array{
        pair{CellFlags::Underline, Decorator::Underline},
        pair{CellFlags::DoublyUnderlined, Decorator::DoubleUnderline},
        pair{CellFlags::CurlyUnderlined, Decorator::CurlyUnderline},
        pair{CellFlags::DottedUnderline, Decorator::DottedUnderline},
        pair{CellFlags::DashedUnderline, Decorator::DashedUnderline},
        pair{CellFlags::Overline, Decorator::Overline},
        pair{CellFlags::CrossedOut, Decorator::CrossedOut},
        pair{CellFlags::Framed, Decorator::Framed},
        pair{CellFlags::Encircled, Decorator::Encircle},
    };

    for (auto const& mapping: mappings)
        if (_cell.flags & mapping.first)
            renderDecoration(mapping.second, gridMetrics_.map(_cell.position), 1, _cell.decorationColor);
}

optional<DecorationRenderer::DataRef> DecorationRenderer::getDataRef(Decorator _decoration)
{
    if (optional<DataRef> const dataRef = atlas_->get(_decoration); dataRef.has_value())
        return dataRef;

    if (atlas_->empty())
        rebuild();

    if (optional<DataRef> const dataRef = atlas_->get(_decoration); dataRef.has_value())
        return dataRef;

    return nullopt;
}

void DecorationRenderer::renderDecoration(Decorator _decoration,
                                          crispy::Point _pos,
                                          int _columnCount,
                                          RGBColor const& _color)
{
    optional<DataRef> const dataRef = getDataRef(_decoration);

    if (!dataRef.has_value())
    {
#if 0 // !defined(NDEBUG)
        cout << fmt::format(
            "DecorationRenderer.renderDecoration: {} from {} with {} cells (MISSING IMPLEMENTATION)\n",
            _decoration, _pos, _columnCount, _color
        );
#endif
        return;
    }

#if 0 // !defined(NDEBUG)
    cout << fmt::format(
        "DecorationRenderer.renderDecoration: {} from {} with {} cells, color {}\n",
        _decoration, _pos, _columnCount, _color
    );
#endif
    auto const x = _pos.x;
    auto const y = _pos.y;
    auto const z = 0;
    auto const color = array{
        float(_color.red) / 255.0f,
        float(_color.green) / 255.0f,
        float(_color.blue) / 255.0f,
        1.0f
    };
    atlas::TextureInfo const& textureInfo = get<0>(dataRef.value()).get();
    auto const advanceX = unbox<int>(gridMetrics_.cellSize.width);
    for (int const i : crispy::times(_columnCount))
        textureScheduler().renderTexture({textureInfo, i * advanceX + x, y, z, color});
}

} // end namespace
