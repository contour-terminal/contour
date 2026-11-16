// SPDX-License-Identifier: Apache-2.0
#include <core/tui/ImageLoader.hpp>

#include <optional>

/// @file
/// Reading an image out of the clipboard, on Windows: not implemented. The POSIX answer asks the
/// desktop's clipboard tool; the Windows one would open the clipboard and take CF_DIBV5, which
/// nothing has needed yet. In endo this was the other arm of an `#ifdef` in
/// `src/tui/ImageLoader.cpp` (f774a210).

namespace core::tui
{

auto readClipboardImage() -> std::optional<ClipboardImage>
{
    return std::nullopt;
}

} // namespace core::tui
