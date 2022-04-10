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
#include <terminal/pty/MockPty.h>

#include <unicode/convert.h>

namespace terminal
{

class MockTerm: public Terminal::Events
{
  public:
    explicit MockTerm(PageSize _size, LineCount _hist = {});

    template <typename Init>
    MockTerm(
        PageSize _size, LineCount _hist = {}, Init init = [](MockTerm&) {}):
        MockTerm { _size, _hist }
    {
        init(*this);
    }

    decltype(auto) pageSize() const noexcept { return terminal.pageSize(); }
    decltype(auto) state() noexcept { return terminal.state(); }
    decltype(auto) state() const noexcept { return terminal.state(); }

    MockPty& mockPty() noexcept { return static_cast<MockPty&>(terminal.device()); }

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

} // namespace terminal
