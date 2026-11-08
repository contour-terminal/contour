// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFontStore.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace vtbackend
{

std::expected<void, SoftFontError> SoftFontStore::define(DecdldParameters const& parameters,
                                                         std::string_view body)
{
    auto const declaration = decodeDecdld(parameters);
    if (!declaration)
        return std::unexpected(declaration.error());

    auto const eraseAll = declaration->erase == SoftFontErase::AllRenditions;
    auto const* const previous = eraseAll ? nullptr : _fonts.at(declaration->fontNumber).get();
    auto font = buildSoftFont(*declaration, body, previous);
    if (!font)
        return std::unexpected(font.error());

    // Only now that the whole sequence is known good is anything touched.
    if (eraseAll)
    {
        _fonts = {};
        _loadedAt = {};
    }
    _fonts.at(declaration->fontNumber) = std::make_shared<SoftFont const>(std::move(*font));
    _loadedAt.at(declaration->fontNumber) = _nextTicket++;
    ++_generation;
    return {};
}

void SoftFontStore::clear() noexcept
{
    _fonts = {};
    _loadedAt = {};
    ++_generation;
}

std::shared_ptr<SoftFont const> SoftFontStore::font(uint8_t number) const noexcept
{
    return number < _fonts.size() ? _fonts[number] : nullptr;
}

std::optional<uint8_t> SoftFontStore::fontForDesignator(std::string_view dscs) const noexcept
{
    auto best = std::optional<uint8_t> {};
    for (auto const number: std::views::iota(size_t { 0 }, _fonts.size()))
        if (_fonts[number] && _fonts[number]->designator == dscs
            && (!best || _loadedAt[number] >= _loadedAt[*best]))
            best = static_cast<uint8_t>(number);
    return best;
}

std::shared_ptr<SoftGlyph const> SoftFontStore::resolve(SoftGlyphRef ref) const noexcept
{
    if (ref.position < FirstSoftGlyphPosition || ref.position > LastSoftGlyphPosition)
        return nullptr;
    auto const loaded = font(ref.font);
    if (!loaded)
        return nullptr;
    auto const& glyph = loaded->glyphs[static_cast<size_t>(ref.position - FirstSoftGlyphPosition)];
    return glyph ? glyph : loaded->errorGlyph;
}

void SoftFontStore::replaceAll(SoftFontSet set)
{
    _fonts = std::move(set.fonts);
    for (auto const number: std::views::iota(size_t { 0 }, _fonts.size()))
        _loadedAt[number] = _fonts[number] ? _nextTicket + set.loadRanks[number] : 0;
    _nextTicket += _fonts.size();
    ++_generation;
}

std::vector<SoftFontRecord> SoftFontStore::capture() const
{
    auto records = std::vector<SoftFontRecord> {};
    auto const ranks = loadRanks();
    for (auto const number: std::views::iota(size_t { 0 }, _fonts.size()))
    {
        auto const& font = _fonts[number];
        if (!font)
            continue;
        auto& record = records.emplace_back(SoftFontRecord {
            .number = static_cast<uint8_t>(number),
            .designator = font->designator,
            .loadRank = ranks[number],
            .startPosition = font->declaration.startPosition,
            .erase = std::to_underlying(font->declaration.erase),
            .declaredWidth = font->declaration.declaredWidth,
            .declaredHeight = font->declaration.declaredHeight,
            .sizeSource = std::to_underlying(font->declaration.sizeSource),
            .columnsPerPage = font->declaration.columnsPerPage,
            .linesPerPage = font->declaration.linesPerPage,
            .usage = std::to_underlying(font->declaration.usage),
            .charsetSize = std::to_underlying(font->declaration.charsetSize),
            .fullWidth = font->geometry.fullWidth,
            .fullHeight = font->geometry.fullHeight,
            .textWidth = font->geometry.textWidth,
            .glyphs = {},
        });
        for (auto const index: std::views::iota(size_t { 0 }, font->glyphs.size()))
            if (auto const& glyph = font->glyphs[index])
                record.glyphs.push_back(
                    SoftGlyphInput { .position = static_cast<uint8_t>(FirstSoftGlyphPosition + index),
                                     .width = glyph->width,
                                     .height = glyph->height,
                                     .coverage = glyph->coverage });
    }
    return records;
}

std::array<uint8_t, SoftFontCount> SoftFontStore::loadRanks() const noexcept
{
    auto ranks = std::array<uint8_t, SoftFontCount> {};
    for (auto const number: std::views::iota(size_t { 0 }, _fonts.size()))
        if (_fonts[number])
            ranks[number] = static_cast<uint8_t>(std::ranges::count_if(
                _loadedAt, [&](uint64_t other) { return other != 0 && other < _loadedAt[number]; }));
    return ranks;
}

std::expected<SoftFontSet, SoftFontError> stageSoftFonts(std::span<SoftFontRecord> records)
{
    auto staged = SoftFontSet {};
    for (auto& record: records)
    {
        if (record.number >= SoftFontCount || staged.fonts[record.number])
            return std::unexpected(SoftFontError::FontNumber);
        if (record.loadRank >= SoftFontCount)
            return std::unexpected(SoftFontError::LoadRank);
        if (record.erase > std::to_underlying(SoftFontErase::AllRenditions))
            return std::unexpected(SoftFontError::EraseControl);
        if (record.sizeSource > std::to_underlying(SoftFontSizeSource::Matrix))
            return std::unexpected(SoftFontError::CellMatrix);
        if (record.usage > std::to_underlying(SoftFontUsage::FullCell))
            return std::unexpected(SoftFontError::Usage);
        if (record.charsetSize > std::to_underlying(SoftCharsetSize::Size96))
            return std::unexpected(SoftFontError::CharsetSize);

        auto const declaration =
            SoftFontDeclaration { .fontNumber = record.number,
                                  .startPosition = record.startPosition,
                                  .erase = static_cast<SoftFontErase>(record.erase),
                                  .declaredWidth = record.declaredWidth,
                                  .declaredHeight = record.declaredHeight,
                                  .sizeSource = static_cast<SoftFontSizeSource>(record.sizeSource),
                                  .columnsPerPage = record.columnsPerPage,
                                  .linesPerPage = record.linesPerPage,
                                  .usage = static_cast<SoftFontUsage>(record.usage),
                                  .charsetSize = static_cast<SoftCharsetSize>(record.charsetSize) };
        auto const geometry = SoftCellGeometry { .fullWidth = record.fullWidth,
                                                 .fullHeight = record.fullHeight,
                                                 .textWidth = record.textWidth };
        auto font = assembleSoftFont(declaration, std::move(record.designator), geometry, record.glyphs);
        if (!font)
            return std::unexpected(font.error());
        staged.fonts[record.number] = std::make_shared<SoftFont const>(std::move(*font));
        staged.loadRanks[record.number] = record.loadRank;
    }
    return staged;
}

} // namespace vtbackend
