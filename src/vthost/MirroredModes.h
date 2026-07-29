// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The modes mirrored over the native protocol — single-sourced so the server's
/// capture (NativeSession) and the client's replay (ScreenMirror) can never
/// disagree on the set. DEC private modes and ANSI modes get one table each.
///
/// **Every mode in this table must be an INDEPENDENT boolean.** That is what makes replaying the
/// set sound: the mirror applies them in table order, so any two modes sharing one underlying value
/// would let the later row overwrite the earlier one's answer.
///
/// The table holds everything that changes how a client ENCODES INPUT and satisfies that rule
/// (cursor keys, keypad, backarrow, bracketed paste, focus reporting) plus cursor visibility.
/// Output-side modes (autowrap, origin, margins, alt-screen) deliberately stay local: the server's
/// emulation already applied them to the cells on the wire, and the mirror manages its own screen
/// with them.
///
/// Input state that is NOT an independent boolean rides its own SessionState/Delta field instead,
/// pull+diffed in NativeSession and asserted by ScreenMirror:
///
///  - The Kitty keyboard protocol (CSI u) — a numeric flag set with its own stack.
///    Carried as `kittyKeyboardFlags`, re-emitted as `CSI = flags ; 1 u` (the
///    set-exactly form; the stack itself stays server-side).
///  - xterm's modifyOtherKeys — carried as `modifyOtherKeys`, re-emitted as
///    `CSI > 4 ; level m` (@see vtbackend/ModifyKeys.h). An app choosing it over
///    CSI u would otherwise get legacy encoding from every client.
///  - **The mouse state.** Nine DEC modes (9/1000/1002/1003 the protocol, 1005/1006/1015/1016 the
///    coordinate encoding, 1007 the wheel) write only THREE values, so they broke the independence
///    rule outright: replaying `{1000 on, 1002 on, 1003 off}` in table order ends with the protocol
///    OFF, because resetting any protocol mode clears whichever one is active (xterm's rule too).
///    That is not a hypothetical — it left the mouse dead on every client that attached while an
///    application was already tracking, and only that client, which is what made it look like an
///    attach-order bug. Carried as resolved `mouseProtocol`/`mouseTransport`/`mouseWheelMode`.

#include <vtbackend/primitives.h>

#include <array>
#include <cstdint>

namespace vthost
{

/// One row per mirrored mode; adding a mode is adding a row — provided it is an independent
/// boolean, per the rule above.
constexpr auto MirroredModes = std::to_array<vtbackend::DECMode>({
    vtbackend::DECMode::UseApplicationCursorKeys, // 1
    vtbackend::DECMode::VisibleCursor,            // 25 (DECTCEM)
    vtbackend::DECMode::ApplicationKeypad,        // 66
    vtbackend::DECMode::BackarrowKey,             // 67
    vtbackend::DECMode::BracketedPaste,           // 2004
    vtbackend::DECMode::FocusTracking,            // 1004
});

/// The ANSI (non-private) modes mirrored, on the same rules as the table above.
///
/// A separate table because they are a separate NUMBER SPACE — ANSI 20 and DEC 20 are different
/// modes — which is also why they ride their own wire field rather than being folded into the DEC
/// one, where nothing could tell them apart.
///
/// Only LNM qualifies, and the other three are excluded for reasons, not by oversight:
///
///  - **IRM (4)** is output-side only. The server's emulation already applied it to the cells on
///    the wire, exactly like autowrap and origin mode.
///  - **KAM (2)** is not an independent boolean: `Terminal::setMode` pushes/pops the INDICATOR
///    STATUS DISPLAY on it, so replaying it would fight the mirror's own status-line handling
///    (@see ScreenMirror::applyStatusDisplay, which exists because the viewer's chrome is not the
///    hosted application's to decide).
///  - **SRM (12)** governs whether the terminal echoes what IT sends, and in attach mode both
///    terminals send: the client's own `flushInput` echo plus the daemon's would double every
///    keystroke. The client keeps the VT default (set: the host echoes), which is what the daemon
///    is doing on its behalf.
constexpr auto MirroredAnsiModes = std::to_array<vtbackend::AnsiMode>({
    vtbackend::AnsiMode::AutomaticNewLine, // 20 (LNM)
});

/// The wire number of the cursor-visibility mode, which the mirror treats
/// specially (it hides the cursor while painting and restores per state).
constexpr uint32_t VisibleCursorModeNumber = 25;

} // namespace vthost
