// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/WindowSizeStack.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/cli/App.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <libunicode/convert.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <optional>

namespace vtbackend
{

/// Reads an instant's UTC fields shifted by @p OffsetMinutes as local civil time, so a test spells the same
/// text in every time zone it runs in.
/// @tparam OffsetMinutes The zone's fixed offset from UTC, in minutes.
/// @param when The instant to convert.
/// @return @p when in that zone's civil time, to the second.
template <int OffsetMinutes = 0>
[[nodiscard]] std::chrono::local_seconds fixedOffsetLocalTime(
    std::chrono::system_clock::time_point when) noexcept
{
    return std::chrono::local_seconds { std::chrono::floor<std::chrono::seconds>(when).time_since_epoch()
                                        + std::chrono::minutes { OffsetMinutes } };
}

template <typename PtyDevice = vtpty::MockPty>
class MockTerm: public Terminal::NullEvents
{
  public:
    MockTerm(ColumnCount columns, LineCount lines): MockTerm { PageSize { lines, columns } } {}

    /// @param size The page size.
    /// @param historyLimits The scrollback's limits.
    /// @param ptyReadBufferSize The PTY read buffer's size.
    /// @param commandServices Who runs `{Command}` status-line items; nullopt renders them empty.
    /// @param localTime The time zone the terminal reads its clocks in: UTC unless a test names another,
    ///                  so that no test depends on the zone of the machine it runs on.
    /// @param linkServices Where passive link detection runs; nullopt detects nothing. Its filesystem is then
    ///                     the terminal's own too.
    explicit MockTerm(PageSize size,
                      HistoryLimits historyLimits = {},
                      size_t ptyReadBufferSize = 1024,
                      std::optional<StatusLineCommandServices> commandServices = std::nullopt,
                      LocalTimeConverter localTime = &fixedOffsetLocalTime<>,
                      std::optional<LinkDetectionServices> linkServices = std::nullopt);

    /// A terminal reading its clocks in the time zone @p localTime.
    MockTerm(PageSize size, LocalTimeConverter localTime):
        MockTerm { size, HistoryLimits {}, 1024, std::nullopt, localTime }
    {
    }

    /// Builds the terminal from @p settings exactly as given, for a test that needs a setting the
    /// other constructors do not expose, such as the parser's BulkScan.
    /// @see createSettings for what the other constructors set.
    /// @param commandServices Who runs `{Command}` status-line items; nullopt renders them empty.
    /// @param localTime The time zone the terminal reads its clocks in, UTC unless a test names another.
    /// @param linkServices Where passive link detection runs; nullopt detects nothing. Its filesystem is then
    ///                     the terminal's own too, so that the paths detection finds and the path under the
    ///                     pointer are looked up in the same place, as Terminal() asks.
    /// @param extensionServices The status-line extensions; nullopt shows none.
    explicit MockTerm(vtbackend::Settings settings,
                      std::optional<StatusLineCommandServices> commandServices = std::nullopt,
                      LocalTimeConverter localTime = &fixedOffsetLocalTime<>,
                      std::optional<LinkDetectionServices> linkServices = std::nullopt,
                      std::optional<StatusLineExtensionServices> extensionServices = std::nullopt);

    /// A terminal that reads printed paths and Git metadata from @p fileSystem; everything else as
    /// `MockTerm { size }`.
    /// @param size The page size.
    /// @param fileSystem The terminal's filesystem. Borrowed: it must outlive the terminal.
    MockTerm(PageSize size, core::platform::FileSystem const& fileSystem):
        MockTerm { createSettings(size, HistoryLimits {}, 1024),
                   core::defaultEnvironment(),
                   fileSystem,
                   std::nullopt,
                   &fixedOffsetLocalTime<>,
                   std::nullopt,
                   std::nullopt }
    {
    }

    /// A terminal reading its environment (`HOME` and the like) from @p environment rather than the
    /// process's, so that a test does not depend on the machine it runs on.
    /// @param size The page size.
    /// @param environment The terminal's environment. Read while the terminal is built.
    MockTerm(PageSize size, core::Environment const& environment):
        MockTerm { createSettings(size, HistoryLimits {}, 1024), environment }
    {
    }

    /// Builds the terminal from @p settings, reading its environment from @p environment.
    /// @param settings The terminal's settings, exactly as given.
    /// @param environment The terminal's environment. Read while the terminal is built.
    MockTerm(vtbackend::Settings settings, core::Environment const& environment):
        MockTerm { std::move(settings),
                   environment,
                   core::platform::NativeFileSystem::instance(),
                   std::nullopt,
                   &fixedOffsetLocalTime<>,
                   std::nullopt,
                   std::nullopt }
    {
    }

    /// A terminal showing the status-line extensions @p extensionServices names.
    MockTerm(PageSize size, StatusLineExtensionServices extensionServices):
        MockTerm { createSettings(size, HistoryLimits {}, 1024),
                   std::nullopt,
                   &fixedOffsetLocalTime<>,
                   std::nullopt,
                   extensionServices }
    {
    }

    /// A scrollback with no headroom, which is what a bare line count has always meant here.
    MockTerm(PageSize size,
             MaxHistoryLineCount maxHistoryLineCount,
             size_t ptyReadBufferSize = 1024,
             std::optional<StatusLineCommandServices> commandServices = std::nullopt):
        MockTerm { size, HistoryLimits::plain(maxHistoryLineCount), ptyReadBufferSize, commandServices }
    {
    }

    template <std::invocable<MockTerm&> Init>
    MockTerm(
        PageSize size, MaxHistoryLineCount hist, size_t ptyReadBufferSize, Init init = [](MockTerm&) {}):
        MockTerm { size, HistoryLimits::plain(hist), ptyReadBufferSize }
    {
        init(*this);
    }

    template <std::invocable<MockTerm&> Init>
    MockTerm(
        PageSize size, HistoryLimits hist, size_t ptyReadBufferSize, Init init = [](MockTerm&) {}):
        MockTerm { size, hist, ptyReadBufferSize }
    {
        init(*this);
    }

    decltype(auto) pageSize() const noexcept { return terminal.pageSize(); }

    PtyDevice& mockPty() noexcept { return static_cast<PtyDevice&>(terminal.device()); }
    PtyDevice const& mockPty() const noexcept { return static_cast<PtyDevice const&>(terminal.device()); }

    void writeToStdin(std::string_view text) { mockPty().stdinBuffer() += text; }

    /// Sends a full press/release pair for @p ch with @p modifiers held, at the time steadyClock reads.
    /// @return whether the press event was handled by the terminal.
    bool sendCharEvent(char32_t ch, KeyboardModifiers modifiers = KeyboardModifiers {})
    {
        return sendCharEvent(ch, modifiers, steadyClock.now());
    }

    /// The same, at @p now rather than at the time steadyClock reads.
    bool sendCharEvent(char32_t ch, KeyboardModifiers modifiers, Terminal::Timestamp now)
    {
        // Synthesize the key identity here, as we don't have a real keyboard to ask.
        auto const keyIdentity =
            KeyIdentity { .unshiftedKey = ch, .nativeVirtualKey = static_cast<uint32_t>(ch) };

        if (!terminal.sendCharEvent(ch, keyIdentity, modifiers, KeyboardEventType::Press, now))
            return false;
        terminal.sendCharEvent(ch, keyIdentity, modifiers, KeyboardEventType::Release, now);
        return true;
    }

    /// Sends a full press/release pair for @p key with @p modifiers held, at the time steadyClock reads.
    /// @return whether the press event was handled by the terminal.
    bool sendKeyEvent(Key key, KeyboardModifiers modifiers = KeyboardModifiers {})
    {
        return sendKeyEvent(key, modifiers, steadyClock.now());
    }

    /// The same, at @p now rather than at the time steadyClock reads.
    bool sendKeyEvent(Key key, KeyboardModifiers modifiers, Terminal::Timestamp now)
    {
        if (!terminal.sendKeyEvent(key, modifiers, KeyboardEventType::Press, now))
            return false;
        terminal.sendKeyEvent(key, modifiers, KeyboardEventType::Release, now);
        return true;
    }

    /// Types @p sequence into stdin, one character event per codepoint, at the time steadyClock reads.
    void sendCharSequence(std::string_view sequence, KeyboardModifiers modifiers = KeyboardModifiers {})
    {
        sendCharSequence(sequence, modifiers, steadyClock.now());
    }

    /// The same, at @p now rather than at the time steadyClock reads.
    void sendCharSequence(std::string_view sequence, KeyboardModifiers modifiers, Terminal::Timestamp now)
    {
        auto const codepoints = unicode::convert_to<char32_t>(sequence);
        for (auto const codepoint: codepoints)
            sendCharEvent(codepoint, modifiers, now);
    }

    void writeToScreen(std::string_view text)
    {
        // Guard the log: core::escape() is a function argument, so it runs whether or not the sink
        // is enabled -- and it std::format()s one string per byte. On a 3 MB sixel frame that was
        // 42% of the whole profile, entirely for a message nobody asked for.
        if (vtpty::ptyOutLog)
            vtpty::ptyOutLog()("writeToScreen: {}", core::escape(text));
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

    void onSelectionCompleted() override { ++selectionCompletions; }

    /// How often the terminal reported a selection completed: what a frontend copies to PRIMARY on.
    int selectionCompletions = 0;

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

    /// The clocks the terminal reads, driven by the test: steadyClock for the terminal's own time and command
    /// durations, wallClock for row birth and block times. Both start at their epoch, which is also the
    /// session epoch row stamps count from. Declared BEFORE `terminal`, which borrows them for its life.
    /// Their times are read in the zone the constructor was handed, UTC by default.
    core::platform::ManualClock steadyClock;
    core::platform::ManualWallClock wallClock;

    /// The token the next DEC mode 2034 reader session is handed. Each session moves it on by one, so no two
    /// sessions share a token; a test that asserts on the exact reply sets it first.
    SemanticBlockToken nextSemanticBlockToken { 1, 2, 3, 4 };

    Terminal terminal;

    std::string clipboardData;
    std::string openedDocument;

    /// The most recent window-frame (tab) color assigned via DECAC item 2, or std::nullopt if the
    /// frame color was reset (or never set). @see setWindowFrameColor, resetWindowFrameColor.
    std::optional<RGBColor> windowFrameColor;
    /// Number of setWindowFrameColor()/resetWindowFrameColor() notifications received.
    int windowFrameColorChangeCount = 0;

    /// The most recent default background announced, or std::nullopt if none was. @see
    /// defaultBackgroundColorChanged.
    std::optional<RGBColor> defaultBackground;
    /// Number of defaultBackgroundColorChanged() notifications received.
    int defaultBackgroundChangeCount = 0;

    // Events overrides
    void setWindowTitle(std::string_view title) override { windowTitle = title; }

    void setIconTitle(std::string_view title) override { iconTitle = title; }

    /// Every pointer-shape notification, in order. An empty entry is the "back to the terminal's own
    /// default" signal, which is what tells a frontend it may resume its own defaults again.
    std::vector<std::string> pointerShapeNotifications;

    void setPointerShape(std::string_view cssName) override
    {
        pointerShapeNotifications.emplace_back(cssName);
    }

    void copyToClipboard(std::string_view data) override { clipboardData = data; }

    std::string getClipboard() override { return clipboardData; }

    /// Records what the terminal asked the desktop to open, so a test can assert on it instead of
    /// only on the absence of a crash.
    void openDocument(std::string_view fileOrUrl) override { openedDocument = fileOrUrl; }

    /// The last resolved link the terminal asked to open as a link (a path at its location); recorded instead
    /// of the default, which hands its target to openDocument().
    std::optional<LinkTarget> openedLink;

    void openLink(LinkTarget const& link) override { openedLink = link; }

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

    void defaultBackgroundColorChanged(RGBColor color) override
    {
        defaultBackground = color;
        ++defaultBackgroundChangeCount;
    }

    /// Every desktop notification raised, as (title, body), in order. Recorded so a test can assert
    /// that a sequence did NOT raise one -- which is how the OSC 9;4 handler proves it no longer
    /// mistakes a progress sequence for a ConEmu notification.
    std::vector<std::pair<std::string, std::string>> notifications;

    void notify(std::string_view title, std::string_view body) override
    {
        notifications.emplace_back(std::string { title }, std::string { body });
    }

    /// Every progress-indicator notification (OSC 9;4), in order. @see Terminal::setProgress.
    std::vector<Progress> progressNotifications;

    void progressChanged(Progress progress) override { progressNotifications.push_back(progress); }

    /// How many times the terminal asked its frontend to open a search prompt -- by `/` in vi mode,
    /// or by the SearchReverse action. There is nothing to type into headlessly, so counting the
    /// requests is the whole of what a test can observe here.
    size_t searchPromptRequests = 0;

    void searchPromptRequested() override { ++searchPromptRequests; }

    /// This machine's identity as the terminal is told it, for the locality of a working directory. Empty
    /// by default, so only an OSC 7 URL with no host is provably local.
    HostIdentity identity;

    /// How often the terminal asked for that identity: once per working directory it resolved. Atomic, as
    /// the terminal may ask on any thread.
    mutable std::atomic<int> localIdentityQueries { 0 };

    [[nodiscard]] LocalIdentity localIdentity() const noexcept override
    {
        ++localIdentityQueries;
        return identity.view();
    }

    /// What new output does to a view scrolled back into the history: it stays by default, as a headless
    /// terminal's view does. @see Terminal::Events::outputFollowing
    OutputFollowing following = OutputFollowing::Stay;

    [[nodiscard]] OutputFollowing outputFollowing() const noexcept override { return following; }

    /// Every command block the terminal announced as finished, in order.
    /// @see Terminal::Events::commandBlockFinished
    std::vector<CommandBlockSummary> finishedCommandBlocks;

    void commandBlockFinished(CommandBlockSummary const& summary) override
    {
        finishedCommandBlocks.push_back(summary);
    }

    /// Every change to a command block's record the terminal told of, in order.
    /// @see Terminal::Events::commandBlockChanged
    std::vector<CommandBlockChange> changedCommandBlocks;

    void commandBlockChanged(CommandBlockChange const& change) override
    {
        changedCommandBlocks.push_back(change);
    }

    /// Every active OSC 3008 context the terminal announced, in order; a zero id is an emptied ancestry.
    /// @see Terminal::Events::contextChanged
    std::vector<ContextId> contextChanges;

    void contextChanged(ContextId activeContext) override { contextChanges.push_back(activeContext); }

    /// How many distinct working directories the terminal announced. @see
    /// Terminal::Events::workingDirectoryChanged
    int workingDirectoryChanges = 0;

    void workingDirectoryChanged() override { ++workingDirectoryChanges; }

    /// Every change to the program-status records the terminal told of, in order.
    /// @see Terminal::Events::programStatusChanged
    std::vector<ProgramStatusChange> programStatusChanges;

    void programStatusChanged(ProgramStatusChange const& change) override
    {
        programStatusChanges.push_back(change);
    }

    /// The screen type of every page the terminal announced as coming on display, in order.
    /// @see Terminal::Events::bufferChanged
    std::vector<ScreenType> bufferChanges;

    void bufferChanged(ScreenType type) override { bufferChanges.push_back(type); }

    static vtbackend::Settings createSettings(PageSize pageSize,
                                              HistoryLimits historyLimits,
                                              size_t ptyReadBufferSize)
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pageSize;
        settings.historyLimits = historyLimits;
        settings.ptyReadBufferSize = ptyReadBufferSize;
        settings.goodImageProtocol = true;
        settings.allowClipboardRead = true; // let tests exercise OSC 52 clipboard reads
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

  private:
    /// What every public constructor comes down to: the Settings one's parameters, plus the terminal's
    /// environment and its filesystem, which must be @p linkServices' own when it is given (as Terminal()
    /// asks).
    MockTerm(vtbackend::Settings settings,
             core::Environment const& environment,
             core::platform::FileSystem const& fileSystem,
             std::optional<StatusLineCommandServices> commandServices,
             LocalTimeConverter localTime,
             std::optional<LinkDetectionServices> linkServices,
             std::optional<StatusLineExtensionServices> extensionServices);
};

template <typename PtyDevice>
inline MockTerm<PtyDevice>::MockTerm(PageSize pageSize,
                                     HistoryLimits historyLimits,
                                     size_t ptyReadBufferSize,
                                     std::optional<StatusLineCommandServices> commandServices,
                                     LocalTimeConverter localTime,
                                     std::optional<LinkDetectionServices> linkServices):
    MockTerm { createSettings(pageSize, historyLimits, ptyReadBufferSize),
               std::move(commandServices),
               localTime,
               std::move(linkServices) }
{
}

template <typename PtyDevice>
inline MockTerm<PtyDevice>::MockTerm(vtbackend::Settings settings,
                                     std::optional<StatusLineCommandServices> commandServices,
                                     LocalTimeConverter localTime,
                                     std::optional<LinkDetectionServices> linkServices,
                                     std::optional<StatusLineExtensionServices> extensionServices):
    MockTerm { std::move(settings),
               core::defaultEnvironment(),
               linkServices ? linkServices->fileSystem : core::platform::NativeFileSystem::instance(),
               std::move(commandServices),
               localTime,
               std::move(linkServices),
               std::move(extensionServices) }
{
}

template <typename PtyDevice>
inline MockTerm<PtyDevice>::MockTerm(vtbackend::Settings settings,
                                     core::Environment const& environment,
                                     core::platform::FileSystem const& fileSystem,
                                     std::optional<StatusLineCommandServices> commandServices,
                                     LocalTimeConverter localTime,
                                     std::optional<LinkDetectionServices> linkServices,
                                     std::optional<StatusLineExtensionServices> extensionServices):
    terminal { *this,
               environment,
               fileSystem,
               std::make_unique<PtyDevice>(settings.pageSize),
               std::move(settings),
               std::move(commandServices),
               TerminalClocks { .steady = steadyClock, .wall = wallClock, .localTime = localTime },
               [this] {
                   auto const token = nextSemanticBlockToken;
                   ++nextSemanticBlockToken.back();
                   return token;
               },
               std::move(linkServices),
               extensionServices }
{
    if (auto const logFilterString = core::defaultEnvironment().get("LOG"))
    {
        core::log::configure(*logFilterString);
        core::cli::App::customizeLogStoreOutput();
    }
}

} // namespace vtbackend
