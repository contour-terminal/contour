// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a session's terminal does with typed text (vtpty::InputEcho) as it travels on the native protocol,
/// in SessionInputEcho -- single-sourced so the daemon's capture and the client's apply cannot disagree.
///
/// Decoding is TOTAL: the byte arrives as a peer chose it, and one this build does not name reads as Unknown,
/// which never locks a pane nor turns secure input on.

#include <vtpty/InputEcho.hpp>

#include <cstdint>
#include <utility>

namespace vthost
{

/// @param echo The echo state.
/// @return Its wire byte: the enumerator's value.
[[nodiscard]] constexpr uint8_t inputEchoToWire(vtpty::InputEcho echo) noexcept
{
    return std::to_underlying(echo);
}

/// @param wire A SessionInputEcho's `echo`.
/// @return The echo state it names; Unknown for a value this build does not know.
[[nodiscard]] constexpr vtpty::InputEcho inputEchoFromWire(uint8_t wire) noexcept
{
    switch (static_cast<vtpty::InputEcho>(wire))
    {
        case vtpty::InputEcho::Unknown:
        case vtpty::InputEcho::Echoed:
        case vtpty::InputEcho::Raw:
        case vtpty::InputEcho::Hidden: return static_cast<vtpty::InputEcho>(wire);
    }
    return vtpty::InputEcho::Unknown;
}

} // namespace vthost
