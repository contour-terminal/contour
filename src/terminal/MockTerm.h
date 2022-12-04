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

#include <terminal/Terminal.h>

#include <vtpty/MockPty.h>

#include <unicode/convert.h>

namespace terminal
{

template <typename PtyDevice = MockPty>
class MockTerm: public Terminal::Events
{
  public:
    explicit MockTerm(PageSize _size, LineCount _hist = {}, size_t ptyReadBufferSize = 1024);

    template <typename Init>
    MockTerm(
        PageSize size, LineCount hist, size_t ptyReadBufferSize, Init init = [](MockTerm&) {}):
        MockTerm { size, hist, ptyReadBufferSize }
    {
        init(*this);
    }

    decltype(auto) pageSize() const noexcept { return terminal.pageSize(); }
    decltype(auto) state() noexcept { return terminal.state(); }
    decltype(auto) state() const noexcept { return terminal.state(); }

    PtyDevice& mockPty() noexcept { return static_cast<PtyDevice&>(terminal.device()); }

    void writeToScreen(std::string_view text)
    {
        mockPty().appendStdOutBuffer(text);
        while (mockPty().isStdoutDataAvailable())
            terminal.processInputOnce();
    }

    void writeToScreen(std::u32string_view text) { writeToScreen(unicode::convert_to<char>(text)); }

    std::string windowTitle;
    Terminal terminal;

    // Events overrides
    void setWindowTitle(std::string_view title) override { windowTitle = title; }
};

template <typename PtyDevice>
inline MockTerm<PtyDevice>::MockTerm(PageSize pageSize,
                                     LineCount maxHistoryLineCount,
                                     size_t ptyReadBufferSize):
    terminal {
        std::make_unique<PtyDevice>(pageSize), 1024 * 1024, ptyReadBufferSize, *this, maxHistoryLineCount
    }
{
}

} // namespace terminal
