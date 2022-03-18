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
    decltype(auto) screen() noexcept { return terminal.screen(); }
    decltype(auto) state() noexcept { return terminal.state(); }
    decltype(auto) state() const noexcept { return terminal.state(); }

    std::string windowTitle;
    Terminal terminal;

    // Events overrides
    void setWindowTitle(std::string_view title) override { windowTitle = title; }
};

} // namespace terminal
