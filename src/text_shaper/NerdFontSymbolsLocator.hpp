// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/FontLocator.hpp>

#include <filesystem>
#include <optional>

namespace text
{

/// A font locator that backs another with the bundled Nerd Fonts symbols font.
///
/// Nerd Fonts' glyphs live in the Unicode private use areas, where no font of the system is the right
/// one: on Windows the system fallback answers `U+E700` and up with Segoe Fluent Icons, whose icons are
/// not the Devicons that code point means to a Nerd Font. So for a run made up only of Nerd Fonts' code
/// points (see isNerdFontSymbol), this locator names the bundled symbols font, and for everything else
/// it is the locator it wraps.
///
/// Only resolve() is affected, which the shapers consult once the fonts configured for the primary
/// font have all failed to cover a run: a font the user chose, and fallbacks they listed, still win.
class NerdFontSymbolsLocator final: public FontLocator
{
  public:
    /// @param inner The locator asked for everything else. It must outlive this one.
    /// @param symbolsFont The symbols font file. Where it is not a file, as in a build tree that was never
    ///                    installed, this locator simply forwards to @p inner.
    NerdFontSymbolsLocator(FontLocator& inner, std::filesystem::path const& symbolsFont);

    /// @return Whether the symbols font was found, so that this locator does more than forward.
    [[nodiscard]] bool hasSymbolsFont() const noexcept { return _symbols.has_value(); }

    [[nodiscard]] CoverageFallbackSpacing coverageFallbackSpacing() const noexcept override;
    [[nodiscard]] FontSourceList all() override;
    [[nodiscard]] FontSourceList locate(FontDescription const& description) override;
    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> codepoints) override;

  private:
    FontLocator& _inner;
    std::optional<FontPath> _symbols;
};

} // namespace text
