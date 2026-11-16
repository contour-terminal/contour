// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::tui
{

/// @brief Terminal cursor shape (DECSCUSR values).
enum class CursorShape : std::uint8_t
{
    Default = 0,           ///< Terminal default cursor shape.
    BlinkingBlock = 1,     ///< Blinking block cursor.
    SteadyBlock = 2,       ///< Steady block cursor.
    BlinkingUnderline = 3, ///< Blinking underline cursor.
    SteadyUnderline = 4,   ///< Steady underline cursor.
    BlinkingBar = 5,       ///< Blinking bar (I-beam) cursor.
    SteadyBar = 6,         ///< Steady bar (I-beam) cursor.
};

} // namespace core::tui
