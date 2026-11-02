// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// Whether mouse events reach an application that asked for them (mouse tracking, the DEC locator).
///
/// Only delivery is governed: the modes an application sets are recorded either way, so forwarding
/// again resumes the protocol the application last asked for.
enum class MouseForwarding : uint8_t
{
    Blocked = 0, ///< The terminal keeps every mouse event for itself (selection, scrolling, links).
    Forwarded,   ///< Mouse events are reported to an application that tracks the mouse.
};

} // namespace vtbackend
