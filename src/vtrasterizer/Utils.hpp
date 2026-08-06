// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Primitives.hpp>

#include <vtrasterizer/TextureAtlas.hpp>

#include <crispy/LogStore.hpp>

namespace vtrasterizer
{

auto inline const rendererLog =
    logstore::Category("vt.renderer", "Logs general information about VT renderer.");
auto inline const rasterizerLog = logstore::Category("vt.rasterizer", "Logs details about text rendering.");

std::vector<uint8_t> downsampleRGBA(std::vector<uint8_t> const& bitmap,
                                    vtbackend::ImageSize size,
                                    vtbackend::ImageSize newSize);

std::vector<uint8_t> downsample(std::vector<uint8_t> const& sourceBitmap,
                                vtbackend::ImageSize targetSize,
                                uint8_t factor);

std::vector<uint8_t> downsample(std::vector<uint8_t> const& bitmap,
                                uint8_t numComponents,
                                vtbackend::ImageSize size,
                                vtbackend::ImageSize newSize);

} // namespace vtrasterizer
