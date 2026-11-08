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

    /// @return The bytes a row of @p width pixels takes.
    constexpr size_t rowBytesOf(size_t width) noexcept
    {
        return (width + 7) / 8;
    }

    /// @return @p glyph's coverage as a wire bitmap: most significant bit leftmost, each row byte-aligned.
    proto::WireSoftGlyph glyphToWire(uint8_t position, vtbackend::SoftGlyph const& glyph)
    {
        auto wire =
            proto::WireSoftGlyph { .position = position, .width = glyph.width, .height = glyph.height };
        auto const rowBytes = rowBytesOf(glyph.width);
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
        auto const rowBytes = rowBytesOf(wire.width);
        for (auto const row: std::views::iota(size_t { 0 }, size_t { wire.height }))
            for (auto const column: std::views::iota(size_t { 0 }, size_t { wire.width }))
                coverage[(row * wire.width) + column] =
                    (std::to_integer<unsigned>(wire.bits[(row * rowBytes) + (column / 8)])
                     >> (7 - (column % 8)))
                    & 1U;
        return coverage;
    }

    /// @return The wire error for a font vtbackend::assembleSoftFont refused.
    constexpr SoftFontWireError wireErrorOf(vtbackend::SoftFontError error) noexcept
    {
        switch (error)
        {
            case vtbackend::SoftFontError::FontNumber: return SoftFontWireError::FontNumber;
            case vtbackend::SoftFontError::Designator: return SoftFontWireError::Designator;
            default: return SoftFontWireError::Glyph;
        }
    }
} // namespace

std::vector<proto::WireSoftFont> softFontsToWire(vtbackend::SoftFontStore const& store)
{
    auto result = std::vector<proto::WireSoftFont> {};
    auto const ranks = store.loadRanks();
    for (auto const number: std::views::iota(size_t { 0 }, store.fonts().size()))
    {
        auto const& font = store.fonts()[number];
        if (!font)
            continue;
        auto& wire = result.emplace_back(proto::WireSoftFont {
            .fontNumber = static_cast<uint8_t>(number),
            .designator = font->designator,
            .charsetSize = std::to_underlying(font->declaration.charsetSize),
            .usage = std::to_underlying(font->declaration.usage),
            .fullWidth = font->geometry.fullWidth,
            .fullHeight = font->geometry.fullHeight,
            .textWidth = font->geometry.textWidth,
            .loadRank = ranks[number],
        });
        for (auto const index: std::views::iota(size_t { 0 }, font->glyphs.size()))
            if (auto const& glyph = font->glyphs[index])
                wire.glyphs.push_back(
                    glyphToWire(static_cast<uint8_t>(proto::FirstSoftGlyphPosition + index), *glyph));
    }
    return result;
}

std::expected<StagedSoftFonts, SoftFontWireError> softFontsFromWire(
    std::span<proto::WireSoftFont const> fonts)
{
    auto staged = StagedSoftFonts {};
    for (auto const& wire: fonts)
    {
        if (wire.fontNumber >= staged.fonts.size() || staged.fonts[wire.fontNumber])
            return std::unexpected(SoftFontWireError::FontNumber);
        if (wire.loadRank >= staged.fonts.size())
            return std::unexpected(SoftFontWireError::LoadRank);
        if (wire.usage > std::to_underlying(vtbackend::SoftFontUsage::FullCell)
            || wire.charsetSize > std::to_underlying(vtbackend::SoftCharsetSize::Size96))
            return std::unexpected(SoftFontWireError::Enumeration);

        // The decoder judged the position range and each bitmap's size. The font itself -- designator,
        // cell, and glyph positions, order and sizes -- is judged by vtbackend::assembleSoftFont, the
        // validator the state codec uses too.
        auto glyphs = std::vector<vtbackend::SoftGlyphInput> {};
        for (auto const& glyph: wire.glyphs)
        {
            if (glyph.bits.size() != rowBytesOf(glyph.width) * glyph.height)
                return std::unexpected(SoftFontWireError::Glyph);
            glyphs.push_back(vtbackend::SoftGlyphInput { .position = glyph.position,
                                                         .width = glyph.width,
                                                         .height = glyph.height,
                                                         .coverage = coverageOf(glyph) });
        }

        auto const declaration = vtbackend::SoftFontDeclaration {
            .fontNumber = wire.fontNumber,
            .usage = static_cast<vtbackend::SoftFontUsage>(wire.usage),
            .charsetSize = static_cast<vtbackend::SoftCharsetSize>(wire.charsetSize),
        };
        auto const geometry = vtbackend::SoftCellGeometry { .fullWidth = wire.fullWidth,
                                                            .fullHeight = wire.fullHeight,
                                                            .textWidth = wire.textWidth };
        auto font = vtbackend::assembleSoftFont(declaration, wire.designator, geometry, glyphs);
        if (!font)
            return std::unexpected(wireErrorOf(font.error()));
        staged.fonts[wire.fontNumber] = std::make_shared<vtbackend::SoftFont const>(std::move(*font));
        staged.loadRanks[wire.fontNumber] = wire.loadRank;
    }
    return staged;
}

} // namespace vthost
