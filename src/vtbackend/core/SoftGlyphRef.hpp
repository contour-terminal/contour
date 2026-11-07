// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>

namespace vtbackend
{

/// Names one glyph of a DECDLD soft font by where it lives, not by what it looks like, so a grid cell
/// stays valid (and cheap) across a redefinition of the font.
struct SoftGlyphRef
{
    uint8_t font = 0;     ///< Pfn, 0..2.
    uint8_t position = 0; ///< 7-bit position in the set, 0x20..0x7F.

    auto operator<=>(SoftGlyphRef const&) const = default;
};

} // namespace vtbackend
