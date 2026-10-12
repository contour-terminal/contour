// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace vtbackend
{

/// Tests whether @p byte is 7-bit ASCII.
/// @param byte The byte to test.
/// @return true if its high bit is clear.
[[nodiscard]] constexpr bool isAsciiByte(char byte) noexcept
{
    return static_cast<unsigned char>(byte) < 0x80;
}

/// Tests whether every byte of @p text is 7-bit ASCII.
///
/// ORs all bytes together and tests the high bit once, instead of stopping at the first non-ASCII
/// byte: without a per-byte exit branch the loop vectorizes. The bulk-write path asks this of runs
/// the parser already found to be text, which are almost always ASCII throughout, so an early exit
/// would almost never be taken anyway.
/// @param text The bytes to test.
/// @return true if no byte has its high bit set; true for an empty text.
[[nodiscard]] inline bool isAsciiOnly(std::string_view text) noexcept
{
    auto bits = 0u;
    for (auto const ch: text)
        bits |= static_cast<unsigned char>(ch);
    return isAsciiByte(static_cast<char>(bits));
}

/// How many bytes asciiPrefixLength() tests at once.
constexpr auto AsciiScanBlockSize = size_t { 32 };

/// Measures the ASCII bytes @p text starts with.
///
/// Tests a block of AsciiScanBlockSize bytes at a time the way isAsciiOnly() does, without a per-byte
/// exit, and walks byte by byte only through the block that holds the first non-ASCII byte.
/// @param text The bytes to measure.
/// @return The offset of the first byte that is not ASCII, or the size of @p text if there is none.
[[nodiscard]] inline size_t asciiPrefixLength(std::string_view text) noexcept
{
    for (auto const blockBegin:
         std::views::iota(size_t { 0 }, text.size()) | std::views::stride(AsciiScanBlockSize))
    {
        auto const block = text.substr(blockBegin, AsciiScanBlockSize);
        if (!isAsciiOnly(block))
            return blockBegin
                   + static_cast<size_t>(std::ranges::find_if_not(block, isAsciiByte) - block.begin());
    }
    return text.size();
}

/// Tests whether a UTF-8 decoder that is idle where @p text begins is idle again where byte @p offset
/// begins, so that an ASCII byte there is a character of its own.
///
/// Mirrors unicode::from_utf8, which vtparser::decodeUtf8Byte decodes text with. A lead byte announces
/// as many bytes as it has leading one bits, two to four, and starts a new character even when the one
/// before is incomplete; every other byte that follows it, ASCII included, is taken as a continuation
/// byte until the character has the bytes its lead announced. So only the last lead byte among the
/// three bytes before @p offset can still be waiting for bytes there.
/// @param text   The bytes of a run, decoded from its first byte on.
/// @param offset An offset into @p text, at most its size.
/// @return true if no character is incomplete where @p offset begins.
[[nodiscard]] constexpr bool isDecoderIdleAt(std::string_view text, size_t offset) noexcept
{
    auto const announcedLength = [](char byte) {
        return static_cast<size_t>(std::countl_one(static_cast<unsigned char>(byte)));
    };
    auto const lookBehindSize = std::min(offset, size_t { 3 });
    auto const lookBehind = text.substr(offset - lookBehindSize, lookBehindSize) | std::views::reverse;
    auto const lead = std::ranges::find_if(
        lookBehind, [&](char byte) { return announcedLength(byte) >= 2 && announcedLength(byte) <= 4; });
    if (lead == lookBehind.end())
        return true;
    auto const bytesSinceLead = static_cast<size_t>(lead - lookBehind.begin()) + 1;
    return bytesSinceLead >= announcedLength(*lead);
}

/// The shortest interior of an ASCII stretch inside mixed text that is worth a bulk write. A shorter
/// one costs more to set up than the per-codepoint path costs to write it. Tuned on the utf8 and
/// utf8sparse throughput workloads: 2 costs dense text more than it saves, 4 does not, and longer
/// minimums leave more of sparse text to the per-codepoint path.
constexpr auto MinimumBulkAsciiStretch = size_t { 4 };

/// Tests whether text is too dense with non-ASCII characters for its ASCII stretches to repay looking
/// for them.
///
/// Every character that is not ASCII takes more bytes than columns: two bytes take at most one column,
/// three or four at most two. So @p byteCount - @p cellCount bounds from above how many such
/// characters there are, each of which can end a stretch. Text that has fewer columns per such
/// character than a stretch with a bulk-written interior needs (MinimumBulkAsciiStretch, plus the byte
/// held back, plus the character itself) is dense. The estimate errs towards dense, which only means
/// the text is written per codepoint, as it was before stretches were written in bulk.
/// @param byteCount How many bytes the text has.
/// @param cellCount How many columns the text takes.
/// @return true if no ASCII stretch is worth looking for.
[[nodiscard]] constexpr bool isTooDenseForBulkAscii(size_t byteCount, size_t cellCount) noexcept
{
    auto const bytesBeyondColumns = byteCount > cellCount ? byteCount - cellCount : size_t { 0 };
    return cellCount < (MinimumBulkAsciiStretch + 2) * bytesBeyondColumns;
}

/// Finds the parts of a run of mixed text that may be written straight into a line as ASCII.
///
/// Calls @p onInterior with the interior of each ASCII stretch of @p text that is long enough to repay
/// a bulk write and begins where the decoder is idle (isDecoderIdleAt), in order. A stretch's first byte
/// is never part of its interior, because it may join the character before it (GB9b: nothing breaks
/// after a Prepend character); its last byte is, because whatever joins it later -- a combining mark,
/// VS16 -- finds it as the last cell written. Every offset handed on is a character boundary, so the
/// bytes between two interiors decode on their own. Calls nothing for text too dense to repay the
/// search (isTooDenseForBulkAscii).
/// @param text       The run, decoded from its first byte on.
/// @param cellCount  How many columns the run takes.
/// @param onInterior Called as onInterior(begin, end) with the offsets of an interior's first byte and
///                   of the byte after its last.
template <typename OnInterior>
constexpr void forEachBulkAsciiInterior(std::string_view text, size_t cellCount, OnInterior const& onInterior)
{
    if (isTooDenseForBulkAscii(text.size(), cellCount))
        return;
    auto position = size_t { 0 };
    while (position < text.size())
    {
        auto const rest = text.substr(position);
        auto const stretchBegin =
            position + static_cast<size_t>(std::ranges::find_if(rest, isAsciiByte) - rest.begin());
        if (stretchBegin == text.size())
            return;
        auto const stretchEnd = stretchBegin + asciiPrefixLength(text.substr(stretchBegin));
        auto const interiorBegin = stretchBegin + 1;
        if (stretchEnd - interiorBegin >= MinimumBulkAsciiStretch && isDecoderIdleAt(text, stretchBegin))
            onInterior(interiorBegin, stretchEnd);
        position = stretchEnd;
    }
}

} // namespace vtbackend
