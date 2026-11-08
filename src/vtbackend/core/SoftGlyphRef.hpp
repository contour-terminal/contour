// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace vtbackend
{

/// How many soft fonts DECDLD has: Pfn 0..2.
inline constexpr size_t SoftFontCount = 3;

/// The positions a grid cell can name: 0x21..0x7E. A font also defines 0x20 and 0x7F in a 96-character
/// set, but a cell showing SP or DEL is a plain one.
inline constexpr uint8_t FirstSoftCellPosition = 0x21;
inline constexpr uint8_t LastSoftCellPosition = 0x7E;

/// Names one glyph of a DECDLD soft font by where it lives, not by what it looks like, so a grid cell
/// stays valid (and cheap) across a redefinition of the font.
struct SoftGlyphRef
{
    uint8_t font = 0;     ///< Pfn, below SoftFontCount.
    uint8_t position = 0; ///< 7-bit position in the set, FirstSoftCellPosition..LastSoftCellPosition.

    auto operator<=>(SoftGlyphRef const&) const = default;
};

} // namespace vtbackend
