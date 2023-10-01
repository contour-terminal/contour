// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Screen.h>

#include <vtrasterizer/RenderTarget.h>

#include <memory>

namespace vtrasterizer
{

class RenderTarget;

class BackgroundRenderer: public Renderable
{
  public:
    /// Constructs the decoration renderer.
    ///
    /// @param gridMetrics
    /// @param defaultColor
    /// @param renderTarget
    BackgroundRenderer(GridMetrics const& gridMetrics, vtbackend::RGBColor const& defaultColor);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;

    constexpr void setOpacity(float value) noexcept { _opacity = static_cast<uint8_t>(value * 255.f); }

    // TODO: pass background color directly (instead of whole grid cell),
    // because there is no need to detect bg/fg color more than once per grid cell!

    /// Queues up a render with given background
    void renderCell(vtbackend::RenderCell const& cell);

    void renderLine(vtbackend::RenderLine const& line);

    void inspect(std::ostream& output) const override;

  private:
    // private data
    vtbackend::RGBColor const& _defaultColor;
    uint8_t _opacity = 255;
};

} // namespace vtrasterizer
