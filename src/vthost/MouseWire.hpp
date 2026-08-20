// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The resolved mouse-reporting state as it travels on the native protocol — single-sourced so the
/// server's capture (NativeSession) and the client's apply (ScreenMirror) cannot disagree on the
/// encoding, exactly as @ref vthost/CursorStyle.h does for DECSCUSR.
///
/// The wire carries the RESOLVED protocol, coordinate encoding and wheel mode rather than the DEC
/// modes that set them, because nine modes write only those three values and several write the
/// same one — so a set of mode numbers cannot say which spelling won. @see vthost/MirroredModes.h
/// for why that disqualifies them from the mirrored-mode table.
///
/// Decoding is total: a value a peer should not have sent degrades to the default rather than
/// being cast into an enum that has no such enumerator.

#include <vtbackend/input/InputGenerator.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace vthost
{

/// Every mouse protocol, which is also every value the wire's `mouseProtocol` may name: the
/// enumerators ARE the DECSET numbers, so the enum is its own encoding table and 0 is free to mean
/// "no protocol active" (no DEC mode is numbered 0).
constexpr auto MouseProtocols = std::to_array({
    vtbackend::MouseProtocol::X10,               // 9
    vtbackend::MouseProtocol::NormalTracking,    // 1000
    vtbackend::MouseProtocol::HighlightTracking, // 1001
    vtbackend::MouseProtocol::ButtonTracking,    // 1002
    vtbackend::MouseProtocol::AnyEventTracking,  // 1003
});

/// @param protocol The active protocol, or nullopt when the application is not tracking.
/// @return Its DECSET number, or 0 for "none".
[[nodiscard]] constexpr uint16_t mouseProtocolNumber(
    std::optional<vtbackend::MouseProtocol> protocol) noexcept
{
    return protocol ? static_cast<uint16_t>(*protocol) : uint16_t {};
}

/// @param number A protocol's DECSET number as the wire spells it.
/// @return The protocol it names, or nullopt when it names none — which covers both the "no
///         protocol active" encoding (0) and a value a peer should not have sent.
[[nodiscard]] constexpr std::optional<vtbackend::MouseProtocol> mouseProtocolOf(uint16_t number) noexcept
{
    auto const it =
        std::ranges::find(MouseProtocols, number, [](auto protocol) { return std::to_underlying(protocol); });
    return it != MouseProtocols.end() ? std::optional { *it } : std::nullopt;
}

/// @param value A MouseTransport as the wire spells it.
/// @return The coordinate encoding it names, falling back to Default for an out-of-range value
///         rather than casting one into the enum.
[[nodiscard]] constexpr vtbackend::MouseTransport mouseTransportOf(uint8_t value) noexcept
{
    return value <= std::to_underlying(vtbackend::MouseTransport::URXVT)
               ? static_cast<vtbackend::MouseTransport>(value)
               : vtbackend::MouseTransport::Default;
}

/// @param value A MouseWheelMode as the wire spells it.
/// @return The wheel mode it names, falling back to Default for an out-of-range value.
[[nodiscard]] constexpr vtbackend::InputGenerator::MouseWheelMode mouseWheelModeOf(uint8_t value) noexcept
{
    using WheelMode = vtbackend::InputGenerator::MouseWheelMode;
    return value <= std::to_underlying(WheelMode::ApplicationCursorKeys) ? static_cast<WheelMode>(value)
                                                                         : WheelMode::Default;
}

} // namespace vthost
