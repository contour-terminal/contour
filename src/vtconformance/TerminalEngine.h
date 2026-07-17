// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Terminal.h>
#include <vtbackend/VTType.h>
#include <vtbackend/WindowSizeStack.h>

#include <vtpty/PageSize.h>
#include <vtpty/Pty.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <vtconformance/Diagnostics.h>
#include <vtconformance/ScreenDump.h>

namespace vtconformance
{

/// A headless `vtbackend::Terminal` plus the frontend behaviours a conformance suite needs: window
/// operations, a clipboard, and the diagnostics oracle.
///
/// It never decides *when* to do I/O, and it holds no clock: it spawns nothing, waits for nothing, and
/// polls nothing. A driver moves bytes through it and the terminal's replies go out through the device
/// it was handed, so the screen is a function of the bytes fed and nothing else.
///
/// Two ways in, and the difference is the point:
///   - `processInputOnce()` reads from the device and feeds the parser in one indivisible step. It is
///     what the pump has always used, and it leaves a driver no seam at which to say "stop here and
///     look at the screen".
///   - `writeToScreen()` takes bytes the caller has already read, so the caller chooses where the
///     stream is cut. Cutting it at a prompt banner is how a golden gets captured causally, rather
///     than by waiting for output to fall quiet and hoping the screen is final.
///
/// @see TerminalHarness, which owns the process.
class TerminalEngine final: public vtbackend::Terminal::NullEvents
{
  public:
    struct Options
    {
        vtpty::PageSize pageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) };

        /// The conformance level Contour reports. vttest picks its test set from DA1, so this
        /// decides how much of the suite is even reachable.
        vtbackend::VTType terminalId = vtbackend::VTType::VT525;

        /// The XTCHECKSUM extension the terminal starts with, and returns to on reset.
        ///
        /// esctest reads the screen back a cell at a time through DECRQCRA, and it cannot interpret a
        /// DEC-default checksum: it needs cells that were never written to read as blanks, and a
        /// cell's video attributes kept out of its value. That is not a Contour quirk -- a real xterm
        /// must be configured exactly the same way (`checksumExtension: 10`) or it fails esctest too.
        vtbackend::ChecksumFlags checksumExtension {};

        /// The size of one character cell, in pixels.
        ///
        /// A headless terminal has no font, so nothing would otherwise fill this in -- and a cell of
        /// zero pixels makes every pixel-denominated report (`CSI 16 t`) and every conversion between
        /// pixels and characters meaningless. This is the engine standing in for the font it does not
        /// have; the exact value does not matter, only that it is fixed and non-zero.
        vtbackend::ImageSize cellPixelSize { vtpty::Width(9), vtpty::Height(18) };

        /// The size of the screen the window sits on, in pixels.
        ///
        /// esctest resizes the window "to the display" and then checks it got there, so an engine with
        /// no display to speak of cannot tell whether the terminal obeyed. This is the engine standing
        /// in for the screen it does not have -- deliberately larger than the window, so that growing to
        /// the display is a change and not a no-op.
        vtbackend::ImageSize screenPixelSize { vtpty::Width(1440), vtpty::Height(900) };
    };

    /// @param device  The PTY the terminal reads from, writes its replies to, and resizes on the
    ///                application's behalf. Ownership is taken -- `vtbackend::Terminal` requires it.
    ///                Injected rather than created so a test can hand in a `vtpty::MockPty` and drive a
    ///                real engine with no child process, and so the terminal's replies and resizes reach
    ///                the real PTY directly, with no relay in between.
    /// @param options Terminal geometry and identity.
    TerminalEngine(std::unique_ptr<vtpty::Pty> device, Options options);
    ~TerminalEngine() override;

    TerminalEngine(TerminalEngine const&) = delete;
    TerminalEngine& operator=(TerminalEngine const&) = delete;
    TerminalEngine(TerminalEngine&&) = delete;
    TerminalEngine& operator=(TerminalEngine&&) = delete;

    /// Feeds @p bytes to the parser, then pushes whatever the terminal replied onto the device.
    ///
    /// Replies are flushed on every call rather than once per PTY read, because `Terminal::reply()`
    /// only QUEUES into the input generator -- a frontend has to push it onto the wire. Forget to, and
    /// every query in every suite comes back unanswered and the report blames the engine for a defect
    /// in its own driver. The deadline is real: vttest's `get_reply()` sleeps 100ms and then polls
    /// FIONREAD exactly once (unix_io.c:160-166), so a late reply is recorded as no reply at all.
    void writeToScreen(std::string_view bytes);

    /// Reads one batch from the device, feeds it to the parser, and pushes any reply back.
    ///
    /// @return Whether the device is still open.
    ///
    /// The read and the feed are one indivisible step, so a caller cannot choose where the byte stream
    /// is cut. @see writeToScreen for the seam the byte-stream driver needs.
    [[nodiscard]] bool processInputOnce();

    /// Presses @p key, encoded by the terminal's OWN input generator, and pushes it onto the device.
    ///
    /// This is what lets vttest's LNM test measure something real. `tst_NLM` (reports.c:585-627) sets
    /// New Line mode, asks for RETURN, and demands CR LF -- then resets it and demands CR alone. That
    /// is a question about the terminal's key encoding (InputGenerator.cpp:241), so a driver that typed
    /// a hard-coded byte would be testing its own string literal instead.
    void pressKey(vtbackend::Key key, vtbackend::KeyboardModifiers modifiers = {});

    /// Writes @p text to the device verbatim, as though it had been typed.
    ///
    /// Bypasses the input generator, so it says nothing about how the terminal encodes a key. Use it
    /// for what a suite wants read back literally (a menu choice); use `pressKey` where the encoding is
    /// the thing under test.
    void writeInput(std::string_view text);

    /// @return The rendered main page, as text.
    [[nodiscard]] std::string screenText() const;

    /// @return A dump of the screen, for comparison against a golden.
    [[nodiscard]] std::string dump(DumpOptions const& options = {}) const;

    /// @return Everything the parser complained about: unknown or unsupported sequences.
    [[nodiscard]] std::vector<Diagnostic> diagnostics() const { return _diagnostics.collected(); }

    /// The device, for the driver to read from and for a test to inspect.
    [[nodiscard]] vtpty::Pty& device() noexcept;

    /// Honours a window resize the application asked for (`CSI 8 ; h ; w t`, DECCOLM, DECSNLS...).
    void requestWindowResize(vtbackend::LineCount lines, vtbackend::ColumnCount columns) override;

    /// Honours an iconify/move request, so the matching report tells the truth afterwards.
    void requestWindowIconify(bool iconify) override;
    void requestWindowMove(vtbackend::WindowPosition position) override;

    /// Honours a pixel-denominated resize (`CSI 4 ; h ; w t`), in whole cells.
    void requestWindowResize(vtbackend::Width width, vtbackend::Height height) override;

    /// Honours a maximize or a full-screen request (`CSI 9 ; Ps t`, `CSI 10 ; Ps t`).
    void requestWindowMaximize(vtbackend::WindowMaximize how) override;
    void requestWindowFullScreen(vtbackend::WindowFullScreen how) override;

    // The headless engine models a clipboard in memory so that OSC 52 write/read round-trips (the
    // conformance suites assume a readable clipboard; a real GUI gates this behind user policy).
    void copyToClipboard(std::string_view data) override { _clipboard = data; }
    std::string getClipboard() override { return _clipboard; }

  private:
    /// Pushes any queued reply onto the device. @see writeToScreen.
    void flushReplies();

    /// Serialises every write to the device.
    ///
    /// The device has two writers whenever a driver pumps on its own thread: the terminal's replies,
    /// flushed from wherever the bytes were fed, and the driver's keystrokes. They share one wire, and
    /// a reply interleaved with a keystroke is a corrupt reply. The engine owns the device, so it owns
    /// this too -- there is no second place a write can come from.
    std::mutex _writeMutex;

    /// @return A timestamp that advances but never consults a clock.
    ///
    /// `Terminal::sendKeyEvent` takes a `Timestamp` and uses it for exactly one thing: resetting the
    /// cursor-blink phase (Terminal.cpp:900-901). The bytes it generates do not depend on it. A real
    /// clock here would put wall-time back into an engine whose whole purpose is to be a pure function
    /// of its input.
    [[nodiscard]] vtbackend::Terminal::Timestamp nextTimestamp() noexcept;

    /// Remembers the size to restore a maximized or full-screen window to. @see vtbackend::WindowSizeStack.
    vtbackend::WindowSizeStack _windowSizeStack;

    /// The clipboard OSC 52 writes to and reads back.
    std::string _clipboard;

    DiagnosticsCollector _diagnostics;
    std::unique_ptr<vtbackend::Terminal> _terminal;

    /// The synthetic clock. @see nextTimestamp.
    vtbackend::Terminal::Timestamp _now {};
};

} // namespace vtconformance
