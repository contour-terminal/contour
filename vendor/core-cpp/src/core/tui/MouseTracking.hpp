// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::tui
{

/// @brief How much mouse input the terminal is asked to report.
///
/// Reporting is opt-in: while a terminal reports the mouse, a click-and-drag there no longer selects
/// text (most terminals keep Shift+drag for selection), so the default asks for nothing. Each mode is
/// a DEC private mode, enabled together with SGR encoding (DEC 1006) so that coordinates are not
/// limited to 223 cells. The parser decodes every mode's reports into a @c MouseEvent.
enum class MouseTracking : std::uint8_t
{
    Off,       ///< No reports (the default).
    Buttons,   ///< Press and release (DEC 1000).
    Drag,      ///< Press, release and motion while a button is held (DEC 1002).
    AnyMotion, ///< Press, release and all motion (DEC 1003).
};

} // namespace core::tui
