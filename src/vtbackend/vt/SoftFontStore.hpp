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
#include <string_view>

namespace vtbackend
{

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

    /// @return The font number whose designator is @p dscs; the most recently loaded wins.
    [[nodiscard]] std::optional<uint8_t> fontForDesignator(std::string_view dscs) const noexcept;

    /// Looks a glyph up.
    /// @return The glyph; the font's error glyph when the font never defined that position; null when the
    ///         font is gone (not loaded, or the ref is out of range: font > 2, position outside
    ///         FirstSoftGlyphPosition..LastSoftGlyphPosition).
    [[nodiscard]] std::shared_ptr<SoftGlyph const> resolve(SoftGlyphRef ref) const noexcept;

    /// @return A counter that increases on every change.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

    /// Replaces every font (state codec and daemon mirror), keeping the order they were loaded in, which
    /// decides which of two fonts sharing a designator fontForDesignator() answers.
    /// @param fonts     The fonts, by number.
    /// @param loadRanks Per number, the font's place in load order, 0 = loaded first. Unused for a null font.
    void replaceAll(std::array<std::shared_ptr<SoftFont const>, SoftFontCount> fonts,
                    std::array<uint8_t, SoftFontCount> loadRanks);

    /// @return Per number, the font's place in load order, 0 = loaded first; 0 for an absent font.
    [[nodiscard]] std::array<uint8_t, SoftFontCount> loadRanks() const noexcept;

    /// @return The loaded fonts by font number; null where none is loaded.
    [[nodiscard]] std::array<std::shared_ptr<SoftFont const>, SoftFontCount> const& fonts() const noexcept
    {
        return _fonts;
    }

  private:
    std::array<std::shared_ptr<SoftFont const>, SoftFontCount> _fonts {};
    std::array<uint64_t, SoftFontCount> _loadedAt {}; ///< Ticket of the load that produced each font.
    uint64_t _nextTicket = 1;
    uint64_t _generation = 0;
};

} // namespace vtbackend
