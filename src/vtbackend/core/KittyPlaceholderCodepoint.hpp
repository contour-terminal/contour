// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace vtbackend::kitty_graphics
{

/// The code point a kitty Unicode placeholder cell starts with.
///
/// In core/ because the grid (its width), the screen (the print path) and the renderer (which never draws
/// it) all name it, besides the placeholder resolver in graphics/.
inline constexpr char32_t PlaceholderCodepoint = U'\U0010EEEE';

} // namespace vtbackend::kitty_graphics
