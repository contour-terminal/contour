// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/SoftGlyphRef.hpp>
#include <vtbackend/vt/SoftFont.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// The fonts of a store, with the order they were loaded in: what a restore or a daemon mirror puts in place
/// whole.
struct SoftFontSet
{
    std::array<std::shared_ptr<SoftFont const>, SoftFontCount> fonts {}; ///< By font number; null = none.
    /// Per number, the font's place in load order, 0 = loaded first. Unused for a null font. The order
    /// decides which of two fonts sharing a designator SoftFontStore::fontForDesignator() answers.
    std::array<uint8_t, SoftFontCount> loadRanks {};
};

/// A font as a state or wire codec carries it: the header fields as raw integers, the glyphs one byte per
/// pixel. Nothing in it is trusted until stageSoftFonts() has judged it.
struct SoftFontRecord
{
    uint8_t number = 0;     ///< Pfn, 0..2.
    std::string designator; ///< Dscs.
    uint8_t loadRank = 0;   ///< 0 = loaded first.
    // The SoftFontDeclaration, enumerators as their numbers.
    uint8_t startPosition = 0x21;
    uint8_t erase = 0;
    uint8_t declaredWidth = 0;
    uint8_t declaredHeight = 0;
    uint8_t sizeSource = 0;
    uint16_t columnsPerPage = 80;
    uint8_t linesPerPage = 24;
    uint8_t usage = 0;
    uint8_t charsetSize = 0;
    // The SoftCellGeometry.
    uint8_t fullWidth = 0;
    uint8_t fullHeight = 0;
    uint8_t textWidth = 0;
    std::vector<SoftGlyphInput> glyphs; ///< The defined positions, ascending.
};

/// The one stager of fonts that come from outside the DECDLD parser (state codec, daemon mirror).
///
/// Judges what a codec's encoding can get wrong -- a font number past the last or named twice, a load rank
/// no number has or two fonts share, an enumerator no enumeration names -- then hands each font to
/// assembleSoftFont, which judges the font itself. Builds aside: nothing is in place until the caller puts
/// the returned set there.
/// @param records The fonts. Consumed: the glyphs' coverage is moved out, even when the set is then refused.
/// @return The fonts by number with their load ranks, or why the set is refused. The reasons are
///         FontNumber, LoadRank, EraseControl, CellMatrix (a size source), Usage and CharsetSize for the
///         encoding, and assembleSoftFont's own for the font.
[[nodiscard]] std::expected<SoftFontSet, SoftFontError> stageSoftFonts(std::span<SoftFontRecord> records);

/// The three DECDLD soft fonts of a terminal.
///
/// Fonts are immutable once published: every load makes a fresh SoftFont, so pointers held elsewhere
/// (render buffers) stay valid and unchanged.
class SoftFontStore
{
  public:
    /// Applies one DECDLD. An invalid sequence leaves the store unchanged and reports why.
    [[nodiscard]] std::expected<void, SoftFontError> define(DecdldParameters const& parameters,
                                                            std::string_view body);

    /// Drops every font (RIS).
    void clear() noexcept;

    /// @return Whether any font is loaded.
    [[nodiscard]] bool hasFonts() const noexcept
    {
        return std::ranges::any_of(_fonts, [](auto const& font) { return font != nullptr; });
    }

    /// @return The font loaded under @p number, or null.
    [[nodiscard]] std::shared_ptr<SoftFont const> font(uint8_t number) const noexcept;

    /// @return The font number whose designator is @p dscs, of whatever set size; the most recently loaded
    ///         wins.
    [[nodiscard]] std::optional<uint8_t> fontForDesignator(std::string_view dscs) const noexcept;

    /// @return The font number whose designator is @p dscs and whose set has @p size (DEC STD 070: the
    ///         same Dscs may name a 94- and a 96-character set, told apart by the SCS that designates it);
    ///         the most recently loaded wins.
    [[nodiscard]] std::optional<uint8_t> fontForDesignator(std::string_view dscs,
                                                           SoftCharsetSize size) const noexcept;

    /// Looks a glyph up.
    /// @return The glyph; the font's error glyph when the font never defined that position; null when the
    ///         font is gone (not loaded, or the ref is out of range: font > 2, position outside
    ///         FirstSoftGlyphPosition..LastSoftGlyphPosition).
    [[nodiscard]] std::shared_ptr<SoftGlyph const> resolve(SoftGlyphRef ref) const noexcept;

    /// @return A counter that increases on every change.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

    /// Replaces every font (state codec and daemon mirror), keeping the order they were loaded in.
    /// @param set The fonts and their load ranks, as stageSoftFonts() returns them.
    void replaceAll(SoftFontSet set);

    /// @return Per number, the font's place in load order, 0 = loaded first; 0 for an absent font.
    [[nodiscard]] std::array<uint8_t, SoftFontCount> loadRanks() const noexcept;

    /// @return Every loaded font as a codec carries it, by font number, with its place in load order.
    [[nodiscard]] std::vector<SoftFontRecord> capture() const;

    /// @return The loaded fonts by font number; null where none is loaded.
    [[nodiscard]] std::array<std::shared_ptr<SoftFont const>, SoftFontCount> const& fonts() const noexcept
    {
        return _fonts;
    }

  private:
    /// @return The most recently loaded font under @p dscs that @p accepts (defined in the .cpp, its only
    /// user).
    template <typename Predicate>
    [[nodiscard]] std::optional<uint8_t> newestWithDesignator(std::string_view dscs,
                                                              Predicate accepts) const noexcept;

    std::array<std::shared_ptr<SoftFont const>, SoftFontCount> _fonts {};
    std::array<uint64_t, SoftFontCount> _loadedAt {}; ///< Ticket of the load that produced each font.
    uint64_t _nextTicket = 1;
    uint64_t _generation = 0;
};

} // namespace vtbackend
