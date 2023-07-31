/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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

#include <vtpty/MockViewPty.h>

#include "crispy/BufferObject.h"

using std::min;
using std::optional;
using std::string_view;
using std::tuple;

namespace terminal
{

void MockViewPty::setReadData(std::string_view data)
{
    assert(_outputBuffer.empty());
    _outputBuffer = data;
}

PtySlave& MockViewPty::slave() noexcept
{
    return _slave;
}

optional<tuple<string_view, bool>> MockViewPty::read(crispy::buffer_object<char>& storage,
                                                     std::chrono::milliseconds /*timeout*/,
                                                     size_t size)
{
    auto const n = min(min(_outputBuffer.size(), storage.bytesAvailable()), size);
    auto result = storage.writeAtEnd(_outputBuffer.substr(0, n));
    _outputBuffer.remove_prefix(n);
    return { tuple { string_view(result.data(), result.size()), false } };
}

void MockViewPty::wakeupReader()
{
    // No-op. as we're a mock-pty.
}

int MockViewPty::write(std::string_view data, bool /*blocking*/)
{
    // Writing into stdin.
    _inputBuffer += std::string_view(data.data(), data.size());
    return static_cast<int>(data.size());
}

terminal::PageSize MockViewPty::pageSize() const noexcept
{
    return _pageSize;
}

void MockViewPty::resizeScreen(terminal::PageSize cells, std::optional<crispy::image_size> pixels)
{
    _pageSize = cells;
    _pixelSize = pixels;
}

void MockViewPty::start()
{
    _closed = false;
}

void MockViewPty::close()
{
    _closed = true;
}

bool MockViewPty::isClosed() const noexcept
{
    return _closed;
}

} // namespace terminal
