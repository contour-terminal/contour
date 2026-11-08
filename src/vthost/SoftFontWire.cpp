// SPDX-License-Identifier: Apache-2.0
#include <vthost/SoftFontWire.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>

namespace vthost
{

namespace
{
    // The protocol is dependency-free and states these numbers itself; they are the DECDLD ones.
    static_assert(proto::MaxSoftFonts == vtbackend::SoftFontCount);
    static_assert(proto::FirstSoftGlyphPosition == vtbackend::FirstSoftGlyphPosition);
    static_assert(proto::LastSoftGlyphPosition == vtbackend::LastSoftGlyphPosition);
    static_assert(proto::FirstSoftCellPosition == vtbackend::FirstSoftCellPosition);
    static_assert(proto::LastSoftCellPosition == vtbackend::LastSoftCellPosition);

    /// @return @p glyph's coverage as a wire bitmap: most significant bit leftmost, each row byte-aligned.
    proto::WireSoftGlyph glyphToWire(vtbackend::SoftGlyphInput const& glyph)
    {
        auto wire =
            proto::WireSoftGlyph { .position = glyph.position, .width = glyph.width, .height = glyph.height };
        auto const rowBytes = proto::softGlyphRowBytes(glyph.width);
        wire.bits.assign(rowBytes * glyph.height, std::byte { 0 });
        for (auto const row: std::views::iota(size_t { 0 }, size_t { glyph.height }))
            for (auto const column: std::views::iota(size_t { 0 }, size_t { glyph.width }))
                if (glyph.coverage[(row * glyph.width) + column] != 0)
                    wire.bits[(row * rowBytes) + (column / 8)] |=
                        static_cast<std::byte>(0x80U >> (column % 8));
        return wire;
    }

    /// @return The one-byte-per-pixel coverage of @p wire, whose bitmap size the decoder has checked.
    std::vector<uint8_t> coverageOf(proto::WireSoftGlyph const& wire)
    {
        auto coverage = std::vector<uint8_t>(size_t { wire.width } * wire.height, uint8_t { 0 });
        auto const rowBytes = proto::softGlyphRowBytes(wire.width);
        for (auto const row: std::views::iota(size_t { 0 }, size_t { wire.height }))
            for (auto const column: std::views::iota(size_t { 0 }, size_t { wire.width }))
                coverage[(row * wire.width) + column] =
                    (std::to_integer<unsigned>(wire.bits[(row * rowBytes) + (column / 8)])
                     >> (7 - (column % 8)))
                    & 1U;
        return coverage;
    }

    /// @return The wire error for a font vtbackend::stageSoftFonts refused.
    constexpr SoftFontWireError wireErrorOf(vtbackend::SoftFontError error) noexcept
    {
        switch (error)
        {
            case vtbackend::SoftFontError::FontNumber: return SoftFontWireError::FontNumber;
            case vtbackend::SoftFontError::LoadRank: return SoftFontWireError::LoadRank;
            case vtbackend::SoftFontError::Designator: return SoftFontWireError::Designator;
            case vtbackend::SoftFontError::EraseControl:
            case vtbackend::SoftFontError::Usage:
            case vtbackend::SoftFontError::CharsetSize: return SoftFontWireError::Enumeration;
            default: return SoftFontWireError::Glyph;
        }
    }
} // namespace

std::vector<proto::WireSoftFont> softFontsToWire(vtbackend::SoftFontStore const& store)
{
    auto result = std::vector<proto::WireSoftFont> {};
    for (auto const& record: store.capture())
    {
        auto& wire = result.emplace_back(proto::WireSoftFont {
            .fontNumber = record.number,
            .designator = record.designator,
            .charsetSize = record.charsetSize,
            .usage = record.usage,
            .fullWidth = record.fullWidth,
            .fullHeight = record.fullHeight,
            .textWidth = record.textWidth,
            .loadRank = record.loadRank,
        });
        for (auto const& glyph: record.glyphs)
            wire.glyphs.push_back(glyphToWire(glyph));
    }
    return result;
}

std::expected<vtbackend::SoftFontSet, SoftFontWireError> softFontsFromWire(
    std::span<proto::WireSoftFont const> fonts)
{
    // The decoder judged the position range and each bitmap's size. The fonts themselves -- number, rank,
    // enumerations, designator, cell, and glyph positions, order and sizes -- are judged by
    // vtbackend::stageSoftFonts, the stager the state codec uses too.
    auto records = std::vector<vtbackend::SoftFontRecord> {};
    for (auto const& wire: fonts)
    {
        auto& record = records.emplace_back(vtbackend::SoftFontRecord { .number = wire.fontNumber,
                                                                        .designator = wire.designator,
                                                                        .loadRank = wire.loadRank,
                                                                        .usage = wire.usage,
                                                                        .charsetSize = wire.charsetSize,
                                                                        .fullWidth = wire.fullWidth,
                                                                        .fullHeight = wire.fullHeight,
                                                                        .textWidth = wire.textWidth,
                                                                        .glyphs = {} });
        for (auto const& glyph: wire.glyphs)
            record.glyphs.push_back(vtbackend::SoftGlyphInput { .position = glyph.position,
                                                                .width = glyph.width,
                                                                .height = glyph.height,
                                                                .coverage = coverageOf(glyph) });
    }
    return vtbackend::stageSoftFonts(records).transform_error(wireErrorOf);
}

} // namespace vthost
