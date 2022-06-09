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

#include <terminal/RenderBuffer.h>
#include <terminal/Screen.h>

#include <terminal_renderer/RenderTarget.h>

#include <memory>

namespace terminal::renderer
{

class RenderTarget;

class BackgroundRenderer: public Renderable
{
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param _gridMetrics
    /// @param _defaultColor
    /// @param _renderTarget
    BackgroundRenderer(GridMetrics const& _gridMetrics, RGBColor const& _defaultColor);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;

    constexpr void setOpacity(float _value) noexcept { opacity_ = static_cast<uint8_t>(_value * 255.f); }

    // TODO: pass background color directly (instead of whole grid cell),
    // because there is no need to detect bg/fg color more than once per grid cell!

    /// Queues up a render with given background
    void renderCell(RenderCell const& _cell);

    void renderLine(RenderLine const& line);

    void inspect(std::ostream& output) const override;

  private:
    // private data
    RGBColor const& defaultColor_;
    uint8_t opacity_ = 255;
};

} // namespace terminal::renderer
