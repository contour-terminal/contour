// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The DECSCUSR cursor style as it travels on the native protocol — single-sourced so the server's
/// capture (NativeSession) and the client's apply (ScreenMirror) cannot disagree on the encoding.
///
/// The wire carries the DECSCUSR `Ps` (1 blinking block … 6 steady bar) rather than Contour's own
/// `CursorShape`/`CursorDisplay` pair, because `Ps` is the vocabulary both ends already share with
/// every application. That makes the mapping lossy in exactly one place — `CursorShape::Rectangle`
/// is a Contour extension DECSCUSR cannot name — so it degrades to block, in one place, with this
/// comment next to it.

#include <vtbackend/core/Primitives.hpp>

#include <array>
#include <cstdint>

namespace vthost
{

/// One row of the DECSCUSR table: the `Ps` and the style it selects.
struct CursorStylePs
{
    uint8_t ps;
    vtbackend::CursorDisplay display;
    vtbackend::CursorShape shape;
};

/// The DECSCUSR styles, in `Ps` order. Adding a shape is adding a row.
///
/// `Ps` 0 is absent on purpose: it means "the configured default", not a style, so it can be
/// received but never sent. @see cursorStyleOf.
constexpr auto CursorStylePsTable = std::array {
    CursorStylePs { 1, vtbackend::CursorDisplay::Blink, vtbackend::CursorShape::Block },
    CursorStylePs { 2, vtbackend::CursorDisplay::Steady, vtbackend::CursorShape::Block },
    CursorStylePs { 3, vtbackend::CursorDisplay::Blink, vtbackend::CursorShape::Underscore },
    CursorStylePs { 4, vtbackend::CursorDisplay::Steady, vtbackend::CursorShape::Underscore },
    CursorStylePs { 5, vtbackend::CursorDisplay::Blink, vtbackend::CursorShape::Bar },
    CursorStylePs { 6, vtbackend::CursorDisplay::Steady, vtbackend::CursorShape::Bar },
};

/// The `Ps` naming a cursor style.
/// @param shape The cursor shape; `Rectangle` degrades to block (DECSCUSR cannot name it).
/// @param display Whether the cursor blinks.
/// @return The DECSCUSR `Ps`, defaulting to 2 (steady block) for a style the table cannot name.
[[nodiscard]] constexpr uint8_t decscusrPs(vtbackend::CursorShape shape,
                                           vtbackend::CursorDisplay display) noexcept
{
    auto const named = shape == vtbackend::CursorShape::Rectangle ? vtbackend::CursorShape::Block : shape;
    for (auto const& row: CursorStylePsTable)
        if (row.shape == named && row.display == display)
            return row.ps;
    return 2;
}

/// The style a `Ps` selects — the inverse of @ref decscusrPs.
/// @param ps The DECSCUSR `Ps` off the wire.
/// @return Its row, or nullptr for `Ps` 0 (restore the configured default) and anything unknown.
[[nodiscard]] constexpr CursorStylePs const* cursorStyleOf(uint8_t ps) noexcept
{
    for (auto const& row: CursorStylePsTable)
        if (row.ps == ps)
            return &row;
    return nullptr;
}

} // namespace vthost
