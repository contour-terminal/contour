// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The DEC private modes mirrored over the native protocol — single-sourced
/// so the server's capture (NativeSession) and the client's replay
/// (ScreenMirror) can never disagree on the set.
///
/// The table holds everything that changes how a client ENCODES INPUT
/// (cursor keys, keypad, backarrow, the mouse protocols, bracketed paste,
/// focus reporting) plus cursor visibility. Output-side modes (autowrap,
/// origin, margins, alt-screen) deliberately stay local: the server's
/// emulation already applied them to the cells on the wire, and the mirror
/// manages its own screen with them.

#include <vtbackend/primitives.h>

#include <array>
#include <cstdint>

namespace muxserver
{

/// One row per mirrored mode; adding a mode is adding a row.
constexpr auto MirroredModes = std::to_array<vtbackend::DECMode>({
    vtbackend::DECMode::UseApplicationCursorKeys,      // 1
    vtbackend::DECMode::VisibleCursor,                 // 25 (DECTCEM)
    vtbackend::DECMode::ApplicationKeypad,             // 66
    vtbackend::DECMode::BackarrowKey,                  // 67
    vtbackend::DECMode::MouseProtocolX10,              // 9
    vtbackend::DECMode::MouseProtocolNormalTracking,   // 1000
    vtbackend::DECMode::MouseProtocolButtonTracking,   // 1002
    vtbackend::DECMode::MouseProtocolAnyEventTracking, // 1003
    vtbackend::DECMode::MouseExtended,                 // 1005
    vtbackend::DECMode::MouseSGR,                      // 1006
    vtbackend::DECMode::MouseURXVT,                    // 1015
    vtbackend::DECMode::MouseSGRPixels,                // 1016
    vtbackend::DECMode::MouseAlternateScroll,          // 1007
    vtbackend::DECMode::BracketedPaste,                // 2004
    vtbackend::DECMode::FocusTracking,                 // 1004
});

/// The wire number of the cursor-visibility mode, which the mirror treats
/// specially (it hides the cursor while painting and restores per state).
constexpr uint32_t VisibleCursorModeNumber = 25;

} // namespace muxserver
