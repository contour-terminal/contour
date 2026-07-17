// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Terminal.h>
#include <vtbackend/WindowSizeStack.h>

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
        // Guard the log: crispy::escape() is a function argument, so it runs whether or not the sink
        // is enabled -- and it std::format()s one string per byte. On a 3 MB sixel frame that was
        // 42% of the whole profile, entirely for a message nobody asked for.
        if (vtpty::ptyOutLog)
            vtpty::ptyOutLog()("writeToScreen: {}", crispy::escape(text));
        mockPty().appendStdOutBuffer(text);
        while (mockPty().isStdoutDataAvailable())
            terminal.processInputOnce();
    }

    /// The frontend's half of a resize the application asked for (DECCOLM, DECSCPP, DECSNLS,
    /// `CSI 8 ; h ; w t`).
    ///
    /// A frontend that silently refuses to resize is not a frontend, and a mock that refuses makes
    /// every sequence that resizes the page untestable.
    ///
    /// Applied on the spot, so a sequence's effect lands where the application put it. Deferring it
    /// would make the resize land wherever the input happened to be split into reads, and the same
    /// byte stream would then produce different screens on different runs.
    ///
    /// The line count names the USABLE main-page area, exactly as the real frontend
    /// (`TerminalDisplay::resizeWindow`) receives it: the status-line height is added back here to form
    /// the total `resizeScreen()` applies. Recording the usable request keeps `requestedPageSize`
    /// comparable to what the sequence asked for (XTWINOPS `CSI 8 t`, DECCOLM, DECSCPP all pass usable).
    void requestWindowResize(LineCount lines, ColumnCount columns) override
    {
        requestedPageSize = PageSize { .lines = lines, .columns = columns };
        if (refuseWindowResize)
            return;
        terminal.resizeScreen(PageSize { .lines = lines + terminal.statusLineHeight(), .columns = columns });
    }

    /// The size most recently requested by the application.
    std::optional<PageSize> requestedPageSize;

    /// When set, the request is recorded but NOT applied — modelling a frontend that cannot honour it
    /// (window maximized/fullscreen, a tiling WM, or the resize still in-flight on the GUI thread). Lets
    /// a test prove a sequence resizes the grid on its own authority rather than via the frontend.
    bool refuseWindowResize = false;

    /// The frontend's half of XTWINOPS's iconify and move.
    ///
    /// A window manager may refuse either, so the terminal never assumes the request took: the frontend
    /// applies it and reports the outcome back. This mock always obliges, which is the simplest frontend
    /// that is still honest about the direction the state flows in.
    void requestWindowIconify(bool iconify) override
    {
        auto state = terminal.windowState();
        state.iconified = iconify;
        terminal.setWindowState(state);
    }

    void requestWindowMove(WindowPosition position) override
    {
        auto state = terminal.windowState();
        state.position = position;
        terminal.setWindowState(state);
    }

    /// A resize named in pixels. A window is only ever a whole number of cells across, so the pixels are
    /// divided by the cell's size -- which is why a frontend with no font cannot honour this at all.
    void requestWindowResize(Width width, Height height) override
    {
        auto const cell = terminal.cellPixelSize();
        if (unbox(cell.width) == 0 || unbox(cell.height) == 0)
            return;

        requestWindowResize(LineCount::cast_from(unbox(height) / unbox(cell.height)),
                            ColumnCount::cast_from(unbox(width) / unbox(cell.width)));
    }

    void requestWindowMaximize(WindowMaximize how) override
    {
        if (auto const size = windowSizeStack.maximize(how, terminal.pageSize(), terminal.screenPageSize()))
            requestWindowResize(size->lines, size->columns);
    }

    void requestWindowFullScreen(WindowFullScreen how) override
    {
        if (auto const size = windowSizeStack.fullScreen(how, terminal.pageSize(), terminal.screenPageSize()))
            requestWindowResize(size->lines, size->columns);
    }

    /// Remembers the size to restore a maximized or full-screen window to. @see WindowSizeStack.
    WindowSizeStack windowSizeStack;

    void writeToScreen(std::u32string_view text) { writeToScreen(unicode::convert_to<char>(text)); }

    std::string windowTitle;

    /// The icon (or tab) title most recently set by the application, via OSC 0 or OSC 1.
    std::string iconTitle;

    Terminal terminal;

    std::string clipboardData;

    /// The most recent window-frame (tab) color assigned via DECAC item 2, or std::nullopt if the
    /// frame color was reset (or never set). @see setWindowFrameColor, resetWindowFrameColor.
    std::optional<RGBColor> windowFrameColor;
    /// Number of setWindowFrameColor()/resetWindowFrameColor() notifications received.
    int windowFrameColorChangeCount = 0;

    // Events overrides
    void setWindowTitle(std::string_view title) override { windowTitle = title; }

    void setIconTitle(std::string_view title) override { iconTitle = title; }

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

    /// Discards every reply queued so far, so that a test can assert on what follows and nothing else.
    ///
    /// Note that this is *not* resetReplyData(): a reply is queued in the terminal's input generator --
    /// where terminal.peekInput() reads it -- and only reaches the PTY's stdin buffer once it is
    /// flushed. Clearing one does not clear the other.
    void discardPendingReplies()
    {
        terminal.flushInput();
        mockPty().stdinBuffer().clear();
    }

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
