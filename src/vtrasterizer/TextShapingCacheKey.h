// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtrasterizer/FontDescriptions.h>

#include <crispy/StrongHash.h>

#include <libunicode/bidi.h>

#include <cstdint>
#include <string_view>

namespace vtrasterizer
{

/// Cache key for a shaped text run.
///
/// Extracted from TextRenderer so that it can be tested on its own: a key that forgets one of its
/// inputs produces wrong glyphs only intermittently, and only under content that mixes the values it
/// forgot -- which is exactly the shape of bug an end-to-end test will not catch.
///
/// Direction is part of the key because it is part of the shaped result. The same codepoints laid
/// out right-to-left yield different glyphs -- Arabic joining forms above all -- so a key without it
/// would serve one run's glyphs to the other.
///
/// @param text      the run's codepoints.
/// @param style     the run's text style.
/// @param direction the run's writing direction.
/// @return a hash distinguishing every combination of the three.
[[nodiscard]] inline crispy::strong_hash hashTextAndStyle(std::u32string_view text,
                                                          TextStyle style,
                                                          unicode::Bidi_Direction direction) noexcept
{
    // Note operator*(strong_hash, uint32_t) mixes rather than multiplies, so a zero-valued enumerator
    // -- TextStyle::Invalid, or Bidi_Direction::Left_To_Right -- still perturbs the hash.
    return crispy::strong_hash::compute(text) * static_cast<uint32_t>(style)
           * static_cast<uint32_t>(direction);
}

} // namespace vtrasterizer
