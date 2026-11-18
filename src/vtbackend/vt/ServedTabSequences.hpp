// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a served session executes (SequenceAllowance::ServedTab): the functions, DEC private modes and C0
/// controls that draw into the grid, as tables. A daemon-scoped extension draws its tab through these and
/// nothing else, so its output reaches no clipboard, title, notification, image, working directory, bell,
/// reply or character set; everything not named here is dropped.

#include <vtbackend/vt/ControlCode.hpp>
#include <vtbackend/vt/Functions.hpp>

#include <algorithm>
#include <array>

namespace vtbackend
{

/// The functions a served session executes: rendition, cursor movement, erasure and editing, margins and
/// scrolling, cursor save and restore, the DEC private modes of ServedTabModes, and the cursor's shape.
inline constexpr auto ServedTabFunctions = std::to_array<Function>({
    // Rendition.
    SGR,
    // Cursor movement.
    CUU,
    CUD,
    CUF,
    CUB,
    CNL,
    CPL,
    CHA,
    CUP,
    HVP,
    VPA,
    HPA,
    // Erasure and editing.
    ED,
    EL,
    ECH,
    DCH,
    ICH,
    IL,
    DL,
    // Margins and scrolling.
    DECSTBM,
    DECSLRM,
    SU,
    SD,
    // Cursor save and restore (DECRC is spelled DECRS here).
    DECSC,
    DECRS,
    // DEC private modes, each parameter filtered by ServedTabModes.
    DECSM,
    DECRM,
    // The cursor's shape.
    DECSCUSR,
});

/// The DEC private modes a served session may set or reset.
inline constexpr auto ServedTabModes = std::to_array<unsigned>({
    7,    // autowrap
    25,   // cursor visible
    47,   // alternate screen
    1047, // alternate screen
    1049, // alternate screen, saving the cursor
    1000, // mouse: button events
    1002, // mouse: button and drag events
    1003, // mouse: any motion
    1006, // mouse: SGR encoding
    1015, // mouse: urxvt encoding
    1016, // mouse: SGR pixel encoding
    1004, // focus events
    2026, // synchronized output
});

/// The C0 controls a served session executes; ESC only as the introducer of a sequence. BEL, ENQ, SO, SI and
/// every other control are dropped.
inline constexpr auto ServedTabControls = std::to_array<ControlCode::C0>({
    ControlCode::C0::BS,
    ControlCode::C0::HT,
    ControlCode::C0::LF,
    ControlCode::C0::VT,
    ControlCode::C0::FF,
    ControlCode::C0::CR,
    ControlCode::C0::ESC,
});

/// @param function A function the parser recognised.
/// @return Whether a served session executes it (@see ServedTabFunctions).
[[nodiscard]] constexpr bool servedTabExecutes(Function const& function) noexcept
{
    return std::ranges::contains(ServedTabFunctions, function);
}

/// @param mode A DEC private mode number, as DECSET/DECRST carry it.
/// @return Whether a served session may set or reset it (@see ServedTabModes).
[[nodiscard]] constexpr bool servedTabSetsMode(unsigned mode) noexcept
{
    return std::ranges::contains(ServedTabModes, mode);
}

/// @param code A C0 control code.
/// @return Whether a served session executes it (@see ServedTabControls).
[[nodiscard]] constexpr bool servedTabExecutesControl(char code) noexcept
{
    return std::ranges::contains(ServedTabControls, static_cast<ControlCode::C0>(code));
}

} // namespace vtbackend
