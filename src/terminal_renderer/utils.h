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

#include <terminal_renderer/Atlas.h>
#include <crispy/debuglog.h>

namespace terminal::renderer {

auto const inline TextRendererTag = crispy::debugtag::make("renderer.text", "Logs details about text rendering.");

std::vector<uint8_t> downsampleRGBA(std::vector<uint8_t> const& _bitmap,
                                    ImageSize _size,
                                    ImageSize _newSize);

std::vector<uint8_t> downsample(std::vector<uint8_t> const& _src,
                                ImageSize _targetSize,
                                uint8_t _factor);

std::vector<uint8_t> downsample(std::vector<uint8_t> const& _bitmap,
                                uint8_t _numComponents,
                                ImageSize _size,
                                ImageSize _newSize);

} // terminal::renderer
