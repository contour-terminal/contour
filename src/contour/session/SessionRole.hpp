// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::session
{

/// What a session's program is to the user. Fixed at construction: it decides how the session's end is
/// reported and whether a saved layout keeps the session's command.
enum class SessionRole : uint8_t
{
    /// A shell, or a program configured to run in its place. One that ends within the early-exit
    /// threshold has likely failed to start, so its pane is held open with a notice; a layout saves its
    /// command.
    Shell = 0,
    /// A pager over a command's output (OpenCommandOutput). Ending at once is how it is used, so its pane
    /// closes when it ends; a layout does not save its command, which names a private file that is gone by
    /// the time the layout is restored.
    CommandOutput,
    /// The terminal Settings shows the profile being edited in: over a canned transcript on a mock device, so
    /// no shell runs and nothing is notified. It is in no window and no layout, has no pane to close and
    /// takes no focus; its owner (SettingsPreview) applies the configuration to it and destroys it.
    Preview,
};

} // namespace contour::session
