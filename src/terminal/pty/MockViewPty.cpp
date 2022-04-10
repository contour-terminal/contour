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

#include <terminal/pty/MockViewPty.h>

using std::optional;
using std::string_view;

namespace terminal
{

void MockViewPty::setReadData(std::string_view _data)
{
    assert(outputBuffer_.empty());
    outputBuffer_ = _data;
}

PtySlave& MockViewPty::slave() noexcept
{
    return slave_;
}

optional<string_view> MockViewPty::read(size_t _maxSize, std::chrono::milliseconds /*_timeout*/)
{
    auto const n = std::min(outputBuffer_.size(), _maxSize);
    auto const result = outputBuffer_.substr(0, n);
    outputBuffer_.remove_prefix(n);
    return result;
}

optional<string_view> MockViewPty::read(crispy::BufferObject& storage, std::chrono::milliseconds timeout)
{
    auto const n = storage.bytesAvailable();
    auto const result = outputBuffer_.substr(0, n);
    outputBuffer_.remove_prefix(n);
    storage.writeAtEnd(result);
    return result;
}

void MockViewPty::wakeupReader()
{
    // No-op. as we're a mock-pty.
}

int MockViewPty::write(char const* buf, size_t size)
{
    // Writing into stdin.
    inputBuffer_ += std::string_view(buf, size);
    return static_cast<int>(size);
}

terminal::PageSize MockViewPty::pageSize() const noexcept
{
    return pageSize_;
}

void MockViewPty::resizeScreen(terminal::PageSize _cells, std::optional<terminal::ImageSize> _pixels)
{
    pageSize_ = _cells;
    pixelSize_ = _pixels;
}

void MockViewPty::close()
{
    closed_ = true;
}

bool MockViewPty::isClosed() const noexcept
{
    return closed_;
}

} // namespace terminal
