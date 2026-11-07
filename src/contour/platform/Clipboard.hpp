// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/ClipboardTarget.hpp>

#include <cstdint>
#include <string_view>

namespace contour::platform
{

/// Whether the desktop has a primary selection besides its clipboard: X11 and Wayland do, macOS and Windows
/// do not.
enum class PrimarySelectionSupport : uint8_t
{
    Absent = 0, ///< Only the clipboard.
    Present,    ///< A primary selection too, what a middle click pastes.
};

/// Decides which of @p targets a desktop can take: all of them, less the primary selection where it has none.
///
/// A copy is not refused for the target that is missing -- `OSC 52 ; cp ; …` still reaches the clipboard on
/// macOS -- and is not moved to the clipboard either, where an application that named only the primary
/// selection would overwrite what the user copied.
/// @param targets Where the copy is to land.
/// @param support Whether the desktop has a primary selection.
/// @return The targets the copy lands in; empty when none of them exists here.
[[nodiscard]] constexpr vtbackend::ClipboardTargets reachableTargets(vtbackend::ClipboardTargets targets,
                                                                     PrimarySelectionSupport support) noexcept
{
    return support == PrimarySelectionSupport::Absent
               ? targets.without(vtbackend::ClipboardTarget::PrimarySelection)
               : targets;
}

/// Puts @p data, as UTF-8 text, on each of @p targets the desktop has (see reachableTargets()).
///
/// The clipboard is an ambient, application-wide resource — hence platform/, not the terminal view that
/// happens to have triggered the copy. Silently does nothing when there is no clipboard (a headless
/// QGuiApplication-less run), which is the same thing the platform does when the user has no session.
///
/// GUI THREAD ONLY: QClipboard is not thread-safe, and callers on the terminal thread must hop first.
/// @param data The text to publish.
/// @param targets Where it lands.
void copyToClipboard(std::string_view data, vtbackend::ClipboardTargets targets);

} // namespace contour::platform
