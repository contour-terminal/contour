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

#include <vtbackend/Terminal.h>

#include <vtpty/MockPty.h>

#include <crispy/App.h>

#include <unicode/convert.h>

namespace terminal
{

template <typename PtyDevice = MockPty>
class MockTerm: public Terminal::Events
{
  public:
    MockTerm(ColumnCount _columns, LineCount _lines): MockTerm { PageSize { _lines, _columns } } {}

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
    PtyDevice const& mockPty() const noexcept { return static_cast<PtyDevice const&>(terminal.device()); }

    void writeToStdin(std::string_view _text) { mockPty().stdinBuffer() += _text; }

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

    static terminal::Settings createSettings(PageSize pageSize,
                                             LineCount maxHistoryLineCount,
                                             size_t ptyReadBufferSize)
    {
        auto settings = terminal::Settings {};
        settings.pageSize = pageSize;
        settings.maxHistoryLineCount = maxHistoryLineCount;
        settings.ptyReadBufferSize = ptyReadBufferSize;
        return settings;
    }

    std::string const& replyData() const noexcept { return mockPty().stdinBuffer(); }

    void requestCaptureBuffer(LineCount lines, bool logical) override
    {
        terminal.primaryScreen().captureBuffer(lines, logical);
    }
};

template <typename PtyDevice>
inline MockTerm<PtyDevice>::MockTerm(PageSize pageSize,
                                     LineCount maxHistoryLineCount,
                                     size_t ptyReadBufferSize):
    terminal { *this,
               std::make_unique<PtyDevice>(pageSize),
               createSettings(pageSize, maxHistoryLineCount, ptyReadBufferSize),
               std::chrono::steady_clock::time_point() } // explicitly start with empty timepoint
{
    char const* logFilterString = getenv("LOG");
    if (logFilterString)
    {
        logstore::configure(logFilterString);
        crispy::App::customizeLogStoreOutput();
    }
}

} // namespace terminal
