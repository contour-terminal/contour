// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Flags.hpp>

#include <cstdint>

namespace vtbackend
{

/// One place a copy can land on the desktop.
///
/// The two are independent -- an application may set either, or both at once (`OSC 52 ; cp ; …`) -- hence
/// bit positions, combined into ClipboardTargets.
enum class ClipboardTarget : uint8_t
{
    Clipboard = 0b01,        ///< The clipboard, what an explicit paste (Ctrl+Shift+V) reads.
    PrimarySelection = 0b10, ///< X11's/Wayland's primary selection, what a middle click pastes.
};

/// The set of places one copy lands. Never empty for a copy that is to happen.
using ClipboardTargets = core::Flags<ClipboardTarget>;

} // namespace vtbackend
