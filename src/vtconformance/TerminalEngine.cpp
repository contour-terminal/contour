// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Screen.h>
#include <vtbackend/Settings.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <vtconformance/TerminalEngine.h>

using namespace std::chrono_literals;

namespace vtconformance
{

namespace
{
    /// How far the synthetic clock advances per key press. @see TerminalEngine::nextTimestamp.
    ///
    /// Any non-zero step does; it only has to move forward, because the one consumer is the
    /// cursor-blink phase.
    constexpr auto TimestampStep = 1ms;
} // namespace

TerminalEngine::TerminalEngine(std::unique_ptr<vtpty::Pty> device, Options options)
{
    auto settings = vtbackend::Settings {};
    settings.terminalId = options.terminalId;
    settings.pageSize = options.pageSize;
    settings.checksumExtension = options.checksumExtension;

    // esctest (and vttest) are authored against xterm's classic environment: a black foreground on a
    // white background. Their colour-reset tests set the live foreground/background to black/white and
    // then expect OSC 110/111 to restore *that* -- i.e. they assume the terminal's default palette is
    // black-on-white. Contour's product default is light-on-dark, so the conformance engine overrides
    // the default palette to match the suites' assumed environment; this is a test-baseline choice, not
    // a change to Contour's real defaults. @see esctest ResetSpecialColorTests.
    using namespace vtbackend;
    settings.colorPalette.defaultForeground = 0x000000_rgb;
    settings.colorPalette.defaultBackground = 0xffffff_rgb;

    // The conformance suites round-trip the clipboard via OSC 52 (write then read); permit reading in
    // the engine. A real GUI keeps this off by default -- clipboard reading is opt-in for the user.
    settings.allowClipboardRead = true;

    _terminal = std::make_unique<vtbackend::Terminal>(
        *this, std::move(device), settings, std::chrono::steady_clock::now());

    // The frontend's half of the window: a headless engine has no font and no screen, so nothing else
    // would fill these in -- and a terminal that reports a cell of zero pixels, or a screen exactly as
    // large as itself, cannot be asked to grow to the display. @see Options.
    _terminal->setCellPixelSize(options.cellPixelSize);
    _terminal->setWindowState(vtbackend::WindowState { .screenPixelSize = options.screenPixelSize });
}

TerminalEngine::~TerminalEngine() = default;

vtpty::Pty& TerminalEngine::device() noexcept
{
    return _terminal->device();
}

vtbackend::Terminal::Timestamp TerminalEngine::nextTimestamp() noexcept
{
    _now += TimestampStep;
    return _now;
}

void TerminalEngine::writeToScreen(std::string_view bytes)
{
    _terminal->writeToScreen(bytes);
    flushReplies();
}

bool TerminalEngine::processInputOnce()
{
    if (!_terminal->processInputOnce())
        return false;

    flushReplies();
    return true;
}

void TerminalEngine::pressKey(vtbackend::Key key, vtbackend::KeyboardModifiers modifiers)
{
    _terminal->sendKeyEvent(key, modifiers, vtbackend::KeyboardEventType::Press, nextTimestamp());
    flushReplies();
}

void TerminalEngine::writeInput(std::string_view text)
{
    auto const guard = std::lock_guard { _writeMutex };
    [[maybe_unused]] auto const written = _terminal->device().write(text);
}

void TerminalEngine::flushReplies()
{
    if (!_terminal->hasInput())
        return;

    auto const guard = std::lock_guard { _writeMutex };
    _terminal->flushInput();
}

void TerminalEngine::requestWindowResize(vtbackend::LineCount lines, vtbackend::ColumnCount columns)
{
    // Applied on the spot, not deferred.
    //
    // Deferring it -- the obvious way to dodge resizing the grid from inside the parser -- makes the
    // resize land wherever the byte stream happened to be cut, so the same stream produces different
    // screens on different runs. That is fatal to a golden-file gate, and it trades a hypothetical
    // hazard for a real one: the parser flushes pending text before it dispatches a sequence, so the
    // grid is never mid-write when this arrives.
    //
    // We are already inside writeToScreen(), which holds the terminal lock, so this must not take it
    // again.
    _terminal->resizeScreen(vtpty::PageSize { .lines = lines, .columns = columns });
}

void TerminalEngine::requestWindowIconify(bool iconify)
{
    auto state = _terminal->windowState();
    state.iconified = iconify;
    _terminal->setWindowState(state);
}

void TerminalEngine::requestWindowMove(vtbackend::WindowPosition position)
{
    auto state = _terminal->windowState();
    state.position = position;
    _terminal->setWindowState(state);
}

void TerminalEngine::requestWindowResize(vtbackend::Width width, vtbackend::Height height)
{
    auto const cell = _terminal->cellPixelSize();
    if (unbox(cell.width) == 0 || unbox(cell.height) == 0)
        return;

    requestWindowResize(vtbackend::LineCount::cast_from(unbox(height) / unbox(cell.height)),
                        vtbackend::ColumnCount::cast_from(unbox(width) / unbox(cell.width)));
}

void TerminalEngine::requestWindowMaximize(vtbackend::WindowMaximize how)
{
    if (auto const size = _windowSizeStack.maximize(how, _terminal->pageSize(), _terminal->screenPageSize()))
        requestWindowResize(size->lines, size->columns);
}

void TerminalEngine::requestWindowFullScreen(vtbackend::WindowFullScreen how)
{
    if (auto const size =
            _windowSizeStack.fullScreen(how, _terminal->pageSize(), _terminal->screenPageSize()))
        requestWindowResize(size->lines, size->columns);
}

std::string TerminalEngine::screenText() const
{
    auto const guard = std::lock_guard { *_terminal };
    return _terminal->currentScreen().renderMainPageText();
}

std::string TerminalEngine::dump(DumpOptions const& options) const
{
    auto const guard = std::lock_guard { *_terminal };
    return dumpScreen(*_terminal, options);
}

} // namespace vtconformance
