// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/SoftGlyphRef.hpp>
#include <vtbackend/vt/SoftFont.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace vtbackend
{

/// What a SoftGlyphRef resolved to.
enum class SoftGlyphLookup : uint8_t
{
    FontGone,  ///< The font is not loaded (or the ref is out of range).
    Undefined, ///< The font is loaded but never defined this position.
    Defined,
};

/// The outcome of SoftFontStore::resolve.
struct SoftGlyphResolution
{
    SoftGlyphLookup lookup = SoftGlyphLookup::FontGone;
    std::shared_ptr<SoftGlyph const> glyph; ///< The error glyph when Undefined; null when FontGone.
};

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

    /// @return The font loaded under @p number, or null.
    [[nodiscard]] std::shared_ptr<SoftFont const> font(uint8_t number) const noexcept;

    /// @return The font number whose designator is @p dscs; the most recently loaded wins.
    [[nodiscard]] std::optional<uint8_t> fontForDesignator(std::string_view dscs) const noexcept;

    /// Looks a glyph up. An out-of-range ref (font > 2, position outside 0x20..0x7F) is FontGone.
    [[nodiscard]] SoftGlyphResolution resolve(SoftGlyphRef ref) const noexcept;

    /// @return A counter that increases on every change.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

    /// Replaces every font (daemon mirror); index order stands in for load order.
    void replaceAll(std::array<std::shared_ptr<SoftFont const>, 3> fonts);

    /// Replaces every font (state codec), keeping the order they were loaded in, which decides which of two
    /// fonts sharing a designator fontForDesignator() answers.
    /// @param fonts     The fonts, by number.
    /// @param loadRanks Per number, the font's place in load order, 0 = loaded first. Unused for a null font.
    void replaceAll(std::array<std::shared_ptr<SoftFont const>, 3> fonts, std::array<uint8_t, 3> loadRanks);

    /// @return Per number, the font's place in load order, 0 = loaded first; 0 for an absent font.
    [[nodiscard]] std::array<uint8_t, 3> loadRanks() const noexcept;

    [[nodiscard]] std::array<std::shared_ptr<SoftFont const>, 3> const& fonts() const noexcept
    {
        return _fonts;
    }

  private:
    std::array<std::shared_ptr<SoftFont const>, 3> _fonts {};
    std::array<uint64_t, 3> _loadedAt {}; ///< Ticket of the load that produced each font.
    uint64_t _nextTicket = 1;
    uint64_t _generation = 0;
};

} // namespace vtbackend
