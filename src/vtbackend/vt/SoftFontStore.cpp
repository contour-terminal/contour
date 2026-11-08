// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFontStore.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    constexpr auto FirstPosition = 0x20;
    constexpr auto LastPosition = 0x7F;
} // namespace

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

SoftGlyphResolution SoftFontStore::resolve(SoftGlyphRef ref) const noexcept
{
    if (ref.position < FirstPosition || ref.position > LastPosition)
        return { .lookup = SoftGlyphLookup::FontGone, .glyph = nullptr };
    auto const loaded = font(ref.font);
    if (!loaded)
        return { .lookup = SoftGlyphLookup::FontGone, .glyph = nullptr };
    auto const& glyph = loaded->glyphs[static_cast<size_t>(ref.position - FirstPosition)];
    if (!glyph)
        return { .lookup = SoftGlyphLookup::Undefined, .glyph = loaded->errorGlyph };
    return { .lookup = SoftGlyphLookup::Defined, .glyph = glyph };
}

void SoftFontStore::replaceAll(std::array<std::shared_ptr<SoftFont const>, 3> fonts)
{
    _fonts = std::move(fonts);
    for (auto const number: std::views::iota(size_t { 0 }, _fonts.size()))
        _loadedAt[number] = _fonts[number] ? _nextTicket++ : 0;
    ++_generation;
}

void SoftFontStore::replaceAll(std::array<std::shared_ptr<SoftFont const>, 3> fonts,
                               std::array<uint8_t, 3> loadRanks)
{
    _fonts = std::move(fonts);
    for (auto const number: std::views::iota(size_t { 0 }, _fonts.size()))
        _loadedAt[number] = _fonts[number] ? _nextTicket + loadRanks[number] : 0;
    _nextTicket += _fonts.size();
    ++_generation;
}

std::array<uint8_t, 3> SoftFontStore::loadRanks() const noexcept
{
    auto ranks = std::array<uint8_t, 3> {};
    for (auto const number: std::views::iota(size_t { 0 }, _fonts.size()))
        if (_fonts[number])
            ranks[number] = static_cast<uint8_t>(std::ranges::count_if(
                _loadedAt, [&](uint64_t other) { return other != 0 && other < _loadedAt[number]; }));
    return ranks;
}

} // namespace vtbackend
