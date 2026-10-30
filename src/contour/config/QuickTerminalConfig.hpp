// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/HotkeyChord.hpp>

#include <cstdint>
#include <optional>

namespace contour::config
{

/// What the quick terminal does when its window loses the focus.
enum class QuickTerminalFocusLoss : uint8_t
{
    Hide = 0, ///< The window slides away.
    Stay,     ///< The window stays until the hotkey is pressed again.
};

/// The `quick_terminal:` section: the hotkey that slides a terminal down from the top, and how it behaves.
///
/// GLOBAL rather than per-profile: there is one quick terminal, so one hotkey.
struct QuickTerminalConfig
{
    /// The hotkey; absent leaves the quick terminal unbound.
    std::optional<HotkeyChord> hotkey {};
    /// What losing the focus does (`quick_terminal.hide_on_focus_loss`).
    QuickTerminalFocusLoss focusLoss = QuickTerminalFocusLoss::Hide;

    [[nodiscard]] bool operator==(QuickTerminalConfig const&) const = default;
};

} // namespace contour::config
