// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalInput.hpp>

#include <core/tui/TerminalProtocols.hpp>

#include <string>

/// @file
/// The @c TerminalInput members that touch no operating-system state, so that the two platform
/// files hold only what genuinely differs: the wait, the raw-mode switch and the protocol write.
/// endo had a copy of each of these in `platform/TerminalInput.cpp` and
/// `platform/TerminalInputWin32.cpp` (f774a210).

namespace core::tui
{

auto TerminalInput::parserTimeout() -> std::vector<InputEvent>
{
    return _parser.timeout();
}

void TerminalInput::setWakeup(core::platform::Wakeup* wakeup)
{
    _wakeup = wakeup;
}

void TerminalInput::suspend()
{
    if (_suspended || !_rawMode)
        return;

    disableProtocols();
    disableRawMode();
    _suspended = true;
}

void TerminalInput::resume()
{
    if (!_suspended)
        return;

    enableRawMode();
    enableProtocols();
    _suspended = false;
}

auto TerminalInput::isSuspended() const noexcept -> bool
{
    return _suspended;
}

void TerminalInput::setAnyMotionTracking(bool enabled)
{
    updateMouseTracking(enabled, _mouseTracking);
}

void TerminalInput::setMouseTracking(MouseTracking mode)
{
    updateMouseTracking(_anyMotionTracking, mode);
}

auto TerminalInput::mouseTracking() const noexcept -> MouseTracking
{
    return _anyMotionTracking ? MouseTracking::AnyMotion : _mouseTracking;
}

void TerminalInput::updateMouseTracking(bool anyMotion, MouseTracking requested)
{
    auto const before = mouseTracking();
    _anyMotionTracking = anyMotion;
    _mouseTracking = requested;
    if (_rawMode)
        writeMouseTrackingChange(before, mouseTracking());
}

void TerminalInput::writeMouseTrackingChange(MouseTracking from, MouseTracking to) const
{
    auto sequence = std::string {};
    protocols::appendMouseTrackingChange(sequence, from, to);
    if (!sequence.empty())
        writeProtocol(sequence);
}

} // namespace core::tui
