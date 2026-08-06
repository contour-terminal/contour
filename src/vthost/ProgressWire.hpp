// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The progress indicator (OSC 9;4) as it travels on the native protocol — single-sourced so the
/// server's capture (NativeSession) and the client's apply (ScreenMirror) cannot disagree, exactly
/// as @ref vthost/StatusWire.hpp does for the status display and @ref vthost/CursorStyle.hpp for
/// DECSCUSR.
///
/// Decoding is TOTAL, for the reason StatusWire.hpp spells out at length: both values arrive as raw
/// bytes a peer chose, and a `ProgressState` no enumerator has would reach the status-line renderer's
/// switch, which ends in `crispy::unreachable()` — undefined behaviour in the attached client.

#include <vtbackend/ProgressState.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace vthost
{

/// @param value A ProgressState as the wire spells it.
/// @return The state it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<vtbackend::ProgressState> progressStateOf(uint8_t value) noexcept
{
    return value <= std::to_underlying(vtbackend::ProgressState::Paused)
               ? std::optional { static_cast<vtbackend::ProgressState>(value) }
               : std::nullopt;
}

/// Rebuilds a whole progress indicator from the two bytes the wire carries.
///
/// @param state      The ProgressState as the wire spells it.
/// @param percentage The percentage as the wire spells it; clamped, since a peer may say anything.
/// @return The indicator, or nullopt when @p state names no enumerator — which the caller treats as
///         "this session says nothing about progress" rather than withdrawing what it already shows.
[[nodiscard]] constexpr std::optional<vtbackend::Progress> progressOf(uint8_t state,
                                                                      uint8_t percentage) noexcept
{
    return progressStateOf(state).transform([percentage](vtbackend::ProgressState decoded) {
        return vtbackend::Progress {
            .state = decoded,
            .percentage = std::min(percentage, vtbackend::MaxProgressPercentage),
        };
    });
}

} // namespace vthost
