// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Terminal.hpp>

#include <windows.h>

/// @file
/// The three @c Terminal members that differ between the platforms, on Windows: a size change
/// arrives as a WINDOW_BUFFER_SIZE_EVENT in the console input buffer, which TerminalInput::poll()
/// reads directly, so there is no signal handler to install or restore. Everything else is in the
/// shared `Terminal.cpp`.

namespace core::tui
{

namespace
{
    /// The initialized input, as the POSIX arm's SIGWINCH handler needs it. Nothing reads it
    /// here -- a size change arrives in the console input buffer -- but it keeps the two arms'
    /// notion of "one Terminal at a time" the same.
    TerminalInput* activeInput = nullptr;
} // namespace

Terminal::~Terminal()
{
    shutdown();
}

auto Terminal::initialize() -> VoidResult
{
    if (_initialized)
        return {};

    // Initialize output first (queries dimensions)
    if (auto result = _output->initialize(); !result)
        return result;

    // In mock mode, skip input initialization and capability queries.
    if (_mockMode)
    {
        _initialized = true;
        return {};
    }

    // Initialize input (raw mode, protocols)
    if (auto result = _input.initialize(); !result)
        return result;

    // No SIGWINCH handler needed on Windows — resize events arrive
    // via WINDOW_BUFFER_SIZE_EVENT in TerminalInput::poll() directly.
    activeInput = &_input;

    // Query cell pixel dimensions (best-effort, non-fatal; unanswered leaves them 0, "unknown")
    if (auto const cellSize = queryCellSize())
    {
        _cellPixelWidth = cellSize->first;
        _cellPixelHeight = cellSize->second;
    }

    // Wait for color scheme response so the first prompt renders with correct colors.
    // The original query from enableProtocols() may have been consumed by the raw
    // XTVERSION read in detectCapabilities(), so re-send it here.
    awaitColorScheme();

    _initialized = true;
    return {};
}

void Terminal::shutdown()
{
    if (!_initialized)
        return;

    if (!_mockMode)
    {
        activeInput = nullptr;
        _input.shutdown();
    }
    _initialized = false;
}

} // namespace core::tui
