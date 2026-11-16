// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Unicode.hpp>

#include <libunicode/utf8_grapheme_segmenter.h>

namespace core::tui
{

int stringWidth(std::string_view text) noexcept
{
    auto width = 0;
    auto segmenter = unicode::utf8_grapheme_segmenter(text);
    for (auto const& cluster: segmenter)
        width += graphemeClusterWidth(cluster);
    return width;
}

} // namespace core::tui
