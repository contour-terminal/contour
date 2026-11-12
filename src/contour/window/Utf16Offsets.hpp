// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a UTF-8 byte offset lands in the same text indexed by UTF-16 code unit. The fuzzy filter reports the
/// characters it matched as UTF-8 byte offsets; QML indexes a string by UTF-16 code unit. For ASCII the two
/// coincide, but every multibyte character shifts the later indices, so without this a picker would bold the
/// wrong characters of a non-ASCII name. Pure and Qt-free.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace contour::window
{

/// Translates ascending UTF-8 byte offsets into UTF-16 code-unit indices of the same text.
/// @param text        The UTF-8 text.
/// @param byteOffsets Ascending byte offsets, each at a code-point boundary.
/// @return The UTF-16 index of each offset, ascending. An offset past the end of @p text, or not on a
///         code-point boundary, is dropped; a negative one is ignored.
[[nodiscard]] inline std::vector<int> byteOffsetsToUtf16(std::string_view text,
                                                         std::span<int const> byteOffsets)
{
    auto result = std::vector<int> {};
    result.reserve(byteOffsets.size());

    auto bytePos = std::size_t { 0 };
    auto utf16Pos = 0;
    for (auto const offset: byteOffsets)
    {
        if (offset < 0)
            continue;
        auto const target = static_cast<std::size_t>(offset);
        // Offsets ascend, so the walk only moves forward, one whole code point at a time: a code point of
        // four bytes is a surrogate pair in UTF-16, every shorter one a single unit.
        while (bytePos < target && bytePos < text.size())
        {
            auto const lead = static_cast<unsigned char>(text[bytePos]);
            auto length = std::size_t { 1 };
            if (lead >= 0xF0)
                length = 4;
            else if (lead >= 0xE0)
                length = 3;
            else if (lead >= 0x80)
                length = 2;
            bytePos += length;
            utf16Pos += length == 4 ? 2 : 1;
        }
        if (bytePos == target)
            result.push_back(utf16Pos);
    }
    return result;
}

} // namespace contour::window
