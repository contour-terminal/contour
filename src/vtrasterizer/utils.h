// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <vtrasterizer/TextureAtlas.h>

#include <crispy/logstore.h>

namespace vtrasterizer
{

auto const inline rendererLog =
    logstore::category("vt.renderer", "Logs general information about VT renderer.");
auto const inline rasterizerLog = logstore::category("vt.rasterizer", "Logs details about text rendering.");

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
