// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The status-display state as it travels on the native protocol — single-sourced so the server's
/// capture (NativeSession) and the client's apply (ScreenMirror) cannot disagree, exactly as
/// @ref vthost/CursorStyle.h does for DECSCUSR and @ref vthost/MouseWire.h for the mouse.
///
/// Decoding is TOTAL, which is the whole reason this file exists. Both values arrive as raw bytes a
/// peer chose, and both were being `static_cast` straight into their enum. A `StatusDisplayType` of
/// 3 then reached `Terminal::statusLineHeight()`, whose switch covers the three real enumerators and
/// then runs `crispy::unreachable()` — undefined behaviour in the attached client (a garbage line
/// count under an optimizing build, a UBSan abort on dev/CI). Every other enum-ish wire field in
/// this protocol already validates; these two are now no exception.

#include <vtbackend/primitives.h>

#include <cstdint>
#include <optional>
#include <utility>

namespace vthost
{

/// @param value A StatusDisplayType as the wire spells it.
/// @return The type it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<vtbackend::StatusDisplayType> statusDisplayTypeOf(
    uint8_t value) noexcept
{
    return value <= std::to_underlying(vtbackend::StatusDisplayType::HostWritable)
               ? std::optional { static_cast<vtbackend::StatusDisplayType>(value) }
               : std::nullopt;
}

/// @param value An ActiveStatusDisplay as the wire spells it.
/// @return The selection it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<vtbackend::ActiveStatusDisplay> activeStatusDisplayOf(
    uint8_t value) noexcept
{
    return value <= std::to_underlying(vtbackend::ActiveStatusDisplay::IndicatorStatusLine)
               ? std::optional { static_cast<vtbackend::ActiveStatusDisplay>(value) }
               : std::nullopt;
}

} // namespace vthost
