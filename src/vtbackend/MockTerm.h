// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Terminal.h>

#include <vtpty/MockPty.h>

#include <crispy/App.h>

#include <libunicode/convert.h>

namespace vtbackend
{

template <typename PtyDevice = vtpty::MockPty>
class MockTerm: public Terminal::NullEvents
{
  public:
    MockTerm(ColumnCount columns, LineCount lines): MockTerm { PageSize { lines, columns } } {}

    explicit MockTerm(PageSize size, LineCount maxHistoryLineCount = {}, size_t ptyReadBufferSize = 1024);

    template <typename Init>
    MockTerm(
        PageSize size, LineCount hist, size_t ptyReadBufferSize, Init init = [](MockTerm&) {}):
        MockTerm { size, hist, ptyReadBufferSize }
    {
        init(*this);
    }

    decltype(auto) pageSize() const noexcept { return terminal.pageSize(); }

    PtyDevice& mockPty() noexcept { return static_cast<PtyDevice&>(terminal.device()); }
    PtyDevice const& mockPty() const noexcept { return static_cast<PtyDevice const&>(terminal.device()); }

    void writeToStdin(std::string_view text) { mockPty().stdinBuffer() += text; }

    bool sendCharEvent(char32_t ch,
                       KeyboardModifiers modifiers = KeyboardModifiers {},
                       Terminal::Timestamp now = std::chrono::steady_clock::now())
    {
        // Simulate physical key here, as we don't have a real keyboard.
        auto const physicalKey = static_cast<uint32_t>(ch);

        if (!terminal.sendCharEvent(ch, physicalKey, modifiers, KeyboardEventType::Press, now))
            return false;
        terminal.sendCharEvent(ch, physicalKey, modifiers, KeyboardEventType::Release, now);
        return true;
    }

    /// Sends a full press/release pair for @p key with @p modifiers held.
    /// @return whether the press event was handled by the terminal.
    bool sendKeyEvent(Key key,
                      KeyboardModifiers modifiers = KeyboardModifiers {},
                      Terminal::Timestamp now = std::chrono::steady_clock::now())
    {
        if (!terminal.sendKeyEvent(key, modifiers, KeyboardEventType::Press, now))
            return false;
        terminal.sendKeyEvent(key, modifiers, KeyboardEventType::Release, now);
        return true;
    }

    // Convenience method to type into stdin a sequence of characters.
    void sendCharSequence(std::string_view sequence,
                          KeyboardModifiers modifiers = KeyboardModifiers {},
                          Terminal::Timestamp now = std::chrono::steady_clock::now())
    {
        auto const codepoints = unicode::convert_to<char32_t>(sequence);
        for (auto const codepoint: codepoints)
            sendCharEvent(codepoint, modifiers, now);
    }

    void writeToScreen(std::string_view text)
    {
        vtpty::ptyOutLog()("writeToScreen: {}", crispy::escape(text));
        mockPty().appendStdOutBuffer(text);
        while (mockPty().isStdoutDataAvailable())
            terminal.processInputOnce();
    }

    void writeToScreen(std::u32string_view text) { writeToScreen(unicode::convert_to<char>(text)); }

    std::string windowTitle;
    Terminal terminal;

    std::string clipboardData;

    /// The most recent window-frame (tab) color assigned via DECAC item 2, or std::nullopt if the
    /// frame color was reset (or never set). @see setWindowFrameColor, resetWindowFrameColor.
    std::optional<RGBColor> windowFrameColor;
    /// Number of setWindowFrameColor()/resetWindowFrameColor() notifications received.
    int windowFrameColorChangeCount = 0;

    // Events overrides
    void setWindowTitle(std::string_view title) override { windowTitle = title; }

    void copyToClipboard(std::string_view data) override { clipboardData = data; }

    void setWindowFrameColor(RGBColor color) override
    {
        windowFrameColor = color;
        ++windowFrameColorChangeCount;
    }

    void resetWindowFrameColor() override
    {
        windowFrameColor = std::nullopt;
        ++windowFrameColorChangeCount;
    }

    static vtbackend::Settings createSettings(PageSize pageSize,
                                              LineCount maxHistoryLineCount,
                                              size_t ptyReadBufferSize)
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pageSize;
        settings.maxHistoryLineCount = maxHistoryLineCount;
        settings.ptyReadBufferSize = ptyReadBufferSize;
        settings.goodImageProtocol = true;
        return settings;
    }

    std::string const& replyData() const noexcept { return mockPty().stdinBuffer(); }
    void resetReplyData() noexcept { mockPty().stdinBuffer().clear(); }

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
        crispy::app::customizeLogStoreOutput();
    }
}

} // namespace vtbackend
