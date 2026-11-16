// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Terminal.hpp>

#include <core/Assert.hpp>
#include <core/platform/Clock.hpp>
#include <core/tui/InputEvent.hpp>

#include <chrono>
#include <tuple>
#include <variant>

/// @file
/// Every @c Terminal member that contains no OS-specific code. Only the three that do --
/// the destructor, @c initialize() and @c shutdown(), which differ in how a size change is
/// delivered -- are in `posix/Terminal.cpp` and `windows/Terminal.cpp`. Keeping the
/// protocol-report policy here (atop the shared @c core::tui::isProtocolReport predicate)
/// ensures POSIX and Windows classify and dispatch terminal reports identically.
///
/// In endo the two platform files each carried a copy of the shared members as well
/// (`platform/Terminal.cpp`, `platform/TerminalWin32.cpp`, f774a210), beside
/// `platform/TerminalShared.cpp`, which this file is the rest of.

namespace core::tui
{

namespace
{
    /// How long a query waits for its reply, on the injected clock.
    constexpr auto QueryReplyTimeout = std::chrono::milliseconds(100);

    /// Passes @p output through, failing the precondition first if it is null: the constructors
    /// ask it its kind in their initializer lists, before any body could check.
    /// @param output The output a Terminal is being constructed over.
    /// @return @p output, never null.
    auto requireOutput(std::unique_ptr<TerminalOutput> output) -> std::unique_ptr<TerminalOutput>
    {
        Require(output != nullptr);
        return output;
    }
} // namespace

Terminal::Terminal():
    _output(std::make_unique<TerminalOutput>()), _clock(core::platform::defaultSteadyClock())
{
}

Terminal::Terminal(std::unique_ptr<TerminalOutput> output):
    _output(requireOutput(std::move(output))),
    _clock(core::platform::defaultSteadyClock()),
    _mockMode(_output->kind() == OutputKind::Simulation)
{
}

Terminal::Terminal(std::unique_ptr<TerminalOutput> output,
                   TerminalQueryInput& queryInput,
                   core::platform::IClock& clock):
    _output(requireOutput(std::move(output))),
    _clock(clock),
    _queryInput(&queryInput),
    _mockMode(_output->kind() == OutputKind::Simulation)
{
}

auto Terminal::canQuery() const noexcept -> bool
{
    return _queryInput != nullptr || !_mockMode;
}

auto Terminal::awaitReport(std::function<bool(InputEvent const&)> const& isReply) -> std::optional<InputEvent>
{
    auto reply = std::optional<InputEvent> {};
    auto others = std::vector<InputEvent> {};
    auto const deadline = _clock.now() + QueryReplyTimeout;

    // The deadline alone ends the wait. An empty poll is not a timeout by itself -- a resize, a
    // wakeup or half of an escape sequence all return early with nothing decoded -- so it loops.
    while (!reply)
    {
        auto const now = _clock.now();
        if (now >= deadline)
            break;

        // Rounded UP: a sub-millisecond remainder truncated to 0 makes the last wait a spin.
        auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        auto events = _queryInput != nullptr ? _queryInput->poll(static_cast<int>(remaining))
                                             : _input.poll(static_cast<int>(remaining));
        for (auto& event: events)
        {
            if (!reply && isReply(event))
                reply = std::move(event);
            else
                others.push_back(std::move(event));
        }
    }

    std::ignore = consumeProtocolReports(others);
    if (!others.empty())
    {
        if (_queryInput != nullptr)
            _queryInput->unread(std::move(others));
        else
            _input.unread(std::move(others));
    }
    return reply;
}

void Terminal::awaitColorScheme()
{
    if (_colorScheme != ColorScheme::Unknown || !canQuery())
        return;

    _output->writeRaw("\033[?996n"); // CSI ? 996 n -- query color scheme
    _output->flush();
    auto reply =
        awaitReport([](InputEvent const& event) { return std::holds_alternative<ColorSchemeReport>(event); });
    if (reply)
    {
        auto events = std::vector<InputEvent> { std::move(*reply) };
        std::ignore = consumeProtocolReports(events);
    }
}

auto Terminal::queryCursorPosition() -> std::expected<std::pair<int, int>, QueryUnanswered>
{
    if (!canQuery())
    {
        if (auto const simulated = _output->simulatedCursorPosition())
            return *simulated;
        return std::unexpected(QueryUnanswered::NotAsked);
    }

    // Send DSR (Device Status Report) to query cursor position.
    // Response will be: CSI row ; col R
    _output->requestCursorPosition();
    _output->flush();

    auto const reply = awaitReport(
        [](InputEvent const& event) { return std::holds_alternative<CursorPositionReport>(event); });
    if (!reply)
        return std::unexpected(QueryUnanswered::NoReply);
    auto const& report = std::get<CursorPositionReport>(*reply);
    return std::pair { report.row, report.column };
}

auto Terminal::queryCellSize() -> std::expected<std::pair<int, int>, QueryUnanswered>
{
    if (!canQuery())
        return std::unexpected(QueryUnanswered::NotAsked);

    // Send CSI 16 t to query cell pixel dimensions.
    // Response will be: CSI 6 ; height ; width t
    _output->requestCellSize();
    _output->flush();

    auto const reply =
        awaitReport([](InputEvent const& event) { return std::holds_alternative<CellSizeReport>(event); });
    if (!reply)
        return std::unexpected(QueryUnanswered::NoReply);
    auto const& report = std::get<CellSizeReport>(*reply);
    return std::pair { report.width, report.height };
}

auto Terminal::queryDecMode(int mode) -> DecModeStatus
{
    if (!canQuery())
        return DecModeStatus::NotAsked;

    // Send DECRQM: CSI ? mode $ p
    // Response: CSI ? mode ; status $ y (DecModeReport)
    //
    // Shared by both platform arms. On Windows the console reads with ENABLE_VIRTUAL_TERMINAL_INPUT,
    // so the reply arrives in the input stream exactly as on POSIX and the same loop reads it. A
    // console that does not answer DECRQM is NoReply at the deadline -- one bounded wait, never a hang.
    _output->requestDecMode(mode);
    _output->flush();

    auto const reply = awaitReport([mode](InputEvent const& event) {
        auto const* report = std::get_if<DecModeReport>(&event);
        return report != nullptr && report->mode == mode;
    });
    if (!reply)
        return DecModeStatus::NoReply;
    return decModeStatusFromReply(std::get<DecModeReport>(*reply).status);
}

auto Terminal::queryDeviceAttributes() -> std::expected<DeviceAttributesReport, QueryUnanswered>
{
    if (!canQuery())
        return std::unexpected(QueryUnanswered::NotAsked);

    // Send DA1: CSI c. Response: CSI ? p1 ; p2 ; ... c
    _output->requestDeviceAttributes();
    _output->flush();

    auto reply = awaitReport(
        [](InputEvent const& event) { return std::holds_alternative<DeviceAttributesReport>(event); });
    if (!reply)
        return std::unexpected(QueryUnanswered::NoReply);
    return std::get<DeviceAttributesReport>(std::move(*reply));
}

bool Terminal::handleFocusEvent(bool focused)
{
    if (focused == _focused)
        return false;

    _focused = focused;
    for (auto const& cb: _focusCallbacks)
        cb(focused);
    return true;
}

bool Terminal::consumeProtocolReports(std::vector<InputEvent>& events)
{
    // Consume protocol-level response events internally — do not pass to application.
    // The color-scheme and focus reports are dispatched to their handlers; the
    // remaining reports (cursor-position, cell-size, DEC-mode, DCS) are dropped,
    // their values having been consumed synchronously by the query methods.
    auto focusChanged = false;
    std::erase_if(events, [&](InputEvent const& event) {
        if (auto const* csr = std::get_if<ColorSchemeReport>(&event))
        {
            auto const scheme = (csr->mode == 2) ? ColorScheme::Light : ColorScheme::Dark;
            handleColorSchemeReport(scheme);
            return true;
        }
        if (auto const* fe = std::get_if<FocusEvent>(&event))
        {
            focusChanged = handleFocusEvent(fe->focused) || focusChanged;
            return true;
        }
        return isProtocolReport(event);
    });
    return focusChanged;
}

auto Terminal::input() noexcept -> TerminalInput&
{
    return _input;
}

auto Terminal::output() noexcept -> TerminalOutput&
{
    return *_output;
}

auto Terminal::poll(int timeoutMs) -> std::vector<InputEvent>
{
    if (_mockMode)
        return {};

    auto events = _input.poll(timeoutMs);
    std::ignore = consumeProtocolReports(events);
    return events;
}

auto Terminal::columns() const noexcept -> int
{
    return _output->columns();
}

auto Terminal::rows() const noexcept -> int
{
    return _output->rows();
}

void Terminal::suspend()
{
    if (_initialized && !_mockMode)
        _input.suspend();
}

void Terminal::resume()
{
    if (_initialized && !_mockMode)
        _input.resume();
}

auto Terminal::isSuspended() const noexcept -> bool
{
    return _input.isSuspended();
}

void Terminal::setMouseTracking(MouseTracking mode)
{
    _input.setMouseTracking(mode);
}

auto Terminal::hudSupported() const noexcept -> bool
{
    return _hudSupported;
}

auto Terminal::cellPixelWidth() const noexcept -> int
{
    return _cellPixelWidth;
}

auto Terminal::cellPixelHeight() const noexcept -> int
{
    return _cellPixelHeight;
}

auto Terminal::colorScheme() const noexcept -> ColorScheme
{
    return _colorScheme;
}

void Terminal::onColorSchemeChanged(std::function<void(ColorScheme)> callback)
{
    _colorSchemeCallbacks.push_back(std::move(callback));
}

void Terminal::handleColorSchemeReport(ColorScheme scheme)
{
    if (scheme == _colorScheme)
        return;

    _colorScheme = scheme;
    for (auto const& cb: _colorSchemeCallbacks)
        cb(scheme);
}

auto Terminal::isFocused() const noexcept -> bool
{
    return _focused;
}

void Terminal::onFocusChanged(std::function<void(bool)> callback)
{
    _focusCallbacks.push_back(std::move(callback));
}

} // namespace core::tui
