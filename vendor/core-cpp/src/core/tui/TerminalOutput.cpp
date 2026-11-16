// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalOutput.hpp>

#include <core/tui/ClipboardProtocol.hpp>
#include <core/tui/SgrBuilder.hpp>
#include <core/tui/TerminalProtocols.hpp>

#include <format>
#include <optional>
#include <string_view>
#include <utility>

/// @file
/// Every @c TerminalOutput member that only composes bytes into the buffer. What
/// touches the operating system — the write itself, the terminal's size, the
/// capability probe and the terminal test — is in `posix/TerminalOutput.cpp` and
/// `windows/TerminalOutput.cpp`, which is all that differs between the two, and on a
/// platform that is neither in `portable/TerminalOutput.cpp`, which needs no operating
/// system. In
/// endo those two files each carried a full copy of this composition (`platform/
/// TerminalOutput.cpp` and `platform/TerminalOutputWin32.cpp`, f774a210), so an
/// escape sequence had to be changed twice.

namespace core::tui
{

using namespace std::string_view_literals;

void TerminalOutput::writeText(std::string_view text, Style const& style)
{
    appendSgr(style);
    _buffer.append(text);
    appendSgrReset();
}

void TerminalOutput::writeRaw(std::string_view text)
{
    _buffer.append(text);
}

void TerminalOutput::moveTo(int row, int col)
{
    _buffer += std::format("\033[{};{}H", row, col);
}

void TerminalOutput::moveUp(int n)
{
    if (n > 0)
        _buffer += std::format("\033[{}A", n);
}

void TerminalOutput::moveDown(int n)
{
    if (n > 0)
        _buffer += std::format("\033[{}B", n);
}

void TerminalOutput::moveLeft(int n)
{
    if (n > 0)
        _buffer += std::format("\033[{}D", n);
}

void TerminalOutput::moveRight(int n)
{
    if (n > 0)
        _buffer += std::format("\033[{}C", n);
}

void TerminalOutput::carriageReturn()
{
    _buffer += '\r';
}

void TerminalOutput::linefeed()
{
    _buffer += '\n';
}

void TerminalOutput::clearToEndOfDisplay()
{
    _buffer += "\033[J";
}

void TerminalOutput::requestCellSize()
{
    _buffer += "\033[16t";
}

void TerminalOutput::requestCursorPosition()
{
    _buffer += "\033[6n";
}

void TerminalOutput::requestDecMode(int mode)
{
    _buffer += std::format("\033[?{}$p", mode);
}

void TerminalOutput::requestDeviceAttributes()
{
    _buffer += protocols::QueryPrimaryDeviceAttributes;
}

void TerminalOutput::clearLine()
{
    _buffer += "\033[2K";
}

void TerminalOutput::clearToEndOfLine()
{
    _buffer += "\033[K";
}

void TerminalOutput::clearToStartOfLine()
{
    _buffer += "\033[1K";
}

void TerminalOutput::deleteLines(int n)
{
    if (n > 0)
        _buffer += std::format("\033[{}M", n);
}

void TerminalOutput::clearScreen()
{
    _buffer += "\033[2J\033[H";
}

void TerminalOutput::clearScrollback()
{
    _buffer += "\033[3J";
}

void TerminalOutput::enterAltScreen()
{
    _buffer += "\033[?1049h";
}

void TerminalOutput::leaveAltScreen()
{
    _buffer += "\033[?1049l";
}

void TerminalOutput::setDoubleWidth()
{
    _buffer += "\033#6";
}

void TerminalOutput::setDoubleHeightTop()
{
    _buffer += "\033#3";
}

void TerminalOutput::setDoubleHeightBottom()
{
    _buffer += "\033#4";
}

void TerminalOutput::setSingleWidth()
{
    _buffer += "\033#5";
}

void TerminalOutput::disableReflow()
{
    _buffer += "\033[?2028l";
}

void TerminalOutput::enableReflow()
{
    _buffer += "\033[?2028h";
}

void TerminalOutput::showCursor()
{
    _buffer += "\033[?25h";
}

void TerminalOutput::hideCursor()
{
    _buffer += "\033[?25l";
}

void TerminalOutput::saveCursor()
{
    // Spelled in two pieces because "\0337" reads as an octal escape followed by a digit.
    _buffer += "\033"
               "7";
}

void TerminalOutput::restoreCursor()
{
    _buffer += "\033"
               "8";
}

void TerminalOutput::setCursorShape(CursorShape shape)
{
    _buffer += std::format("\033[{} q", static_cast<int>(shape));
}

void TerminalOutput::setScrollRegion(int top, int bottom)
{
    _buffer += std::format("\033[{};{}r", top, bottom);
}

void TerminalOutput::resetScrollRegion()
{
    _buffer += "\033[r";
}

void TerminalOutput::writeSixel(std::string_view sixelData)
{
    _buffer += "\033P0;1q";
    _buffer.append(sixelData);
    _buffer += "\033\\";
}

void TerminalOutput::copyToClipboard(std::string_view text)
{
    appendOsc52(_buffer, text, ClipboardTarget::Clipboard);
}

void TerminalOutput::unscroll(int n)
{
    // Kitty unscroll extension: CSI Ps + T
    // This is an extension to SD (Scroll Down / Pan Up) that restores
    // lines from the scrollback buffer instead of inserting blank lines.
    // See: https://sw.kovidgoyal.net/kitty/unscroll/
    if (n > 0)
        _buffer += std::format("\033[{}+T", n);
}

bool TerminalOutput::supportsUnscroll() const noexcept
{
    return _unscrollSupported;
}

void TerminalOutput::beginHyperlink(std::string_view url, std::string_view id)
{
    protocols::appendHyperlinkOpen(_buffer, url, id);
}

void TerminalOutput::endHyperlink()
{
    _buffer += protocols::HyperlinkClose;
}

void TerminalOutput::writeHyperlink(std::string_view text, std::string_view url, Style const& style)
{
    beginHyperlink(url, {});
    writeText(text, style);
    endHyperlink();
}

auto TerminalOutput::syncGuard() -> SyncGuard
{
    return SyncGuard(*this); // The flush on the way in is the guard's constructor's.
}

void TerminalOutput::flush()
{
    if (!_buffer.empty())
    {
        writeToDestination(_buffer);
        _buffer.clear();
    }
}

auto TerminalOutput::kind() const noexcept -> OutputKind
{
    return OutputKind::Device;
}

auto TerminalOutput::simulatedCursorPosition() const noexcept -> std::optional<std::pair<int, int>>
{
    return std::nullopt;
}

auto TerminalOutput::columns() const noexcept -> int
{
    return _cols;
}

auto TerminalOutput::rows() const noexcept -> int
{
    return _rows;
}

void TerminalOutput::appendSgr(Style const& style)
{
    _buffer += buildSgrSequence(style);
}

void TerminalOutput::appendSgrReset()
{
    _buffer += protocols::SgrReset;
}

void TerminalOutput::resetStyle()
{
    appendSgrReset();
}

// --- SyncGuard ---
//
// The sequences go through TerminalOutput::writeToDestination() rather than to a native handle,
// so they land wherever that output's bytes land. Writing them directly to the process's standard
// output, as endo did (src/tui/platform/TerminalOutput.cpp:355-359 at f774a210), put the begin and
// end of a retargeted output's synchronised frame on a stream that never saw the frame's contents.

namespace
{
    constexpr auto BeginSynchronizedOutput = "[?2026h"sv;
    constexpr auto EndSynchronizedOutput = "[?2026l"sv;
} // namespace

// Whether to bracket is the destination's answer, asked once, here: a pipe or a file gets the frame
// without `CSI ? 2026 h` / `l` around it, as it gets text without colour. Every caller used to ask
// isTerminal() itself and choose between a guard and none. The flushes stay unconditional, because
// a caller that composed inside the region relies on the guard to write it out, on a pipe too.
SyncGuard::SyncGuard(TerminalOutput& output): _output(&output), _bracketed(output.isTerminal())
{
    // Flush first, as the destructor does: what was composed BEFORE the region belongs before it.
    // Left buffered, it would be emitted inside the synchronized region on the next flush -- the
    // frame would carry bytes that are not its own, which is the tearing this class exists to
    // prevent, and the natural RAII spelling `auto guard = SyncGuard { output };` would be the one
    // that got it wrong.
    _output->flush();
    if (_bracketed)
        _output->writeToDestination(BeginSynchronizedOutput);
}

SyncGuard::~SyncGuard()
{
    end();
}

SyncGuard::SyncGuard(SyncGuard&& other) noexcept:
    _output(std::exchange(other._output, nullptr)), _bracketed(std::exchange(other._bracketed, false))
{
}

auto SyncGuard::operator=(SyncGuard&& other) noexcept -> SyncGuard&
{
    if (this != &other)
    {
        end();
        _output = std::exchange(other._output, nullptr);
        _bracketed = std::exchange(other._bracketed, false);
    }
    return *this;
}

void SyncGuard::end() noexcept
{
    if (_output == nullptr)
        return;
    // Flush first: anything composed inside the region and still buffered would otherwise be
    // emitted after the end sequence, landing outside the very region it was composed in. The
    // constructor flushes on the way in for the same reason, so the two ends match.
    _output->flush();
    if (_bracketed)
        _output->writeToDestination(EndSynchronizedOutput);
}

} // namespace core::tui
