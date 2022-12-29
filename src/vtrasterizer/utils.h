/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

#include <vtbackend/primitives.h>

#include <vtrasterizer/TextureAtlas.h>

#include <crispy/logstore.h>

namespace terminal::rasterizer
{

auto const inline RendererLog =
    logstore::Category("vt.renderer", "Logs general information about VT renderer.");
auto const inline RasterizerLog = logstore::Category("vt.rasterizer", "Logs details about text rendering.");

std::vector<uint8_t> downsampleRGBA(std::vector<uint8_t> const& bitmap, ImageSize size, ImageSize newSize);

std::vector<uint8_t> downsample(std::vector<uint8_t> const& src, ImageSize targetSize, uint8_t factor);

std::vector<uint8_t> downsample(std::vector<uint8_t> const& bitmap,
                                uint8_t numComponents,
                                ImageSize size,
                                ImageSize newSize);

} // namespace terminal::rasterizer
