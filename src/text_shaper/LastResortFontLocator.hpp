// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/FontLocator.hpp>

#include <cstdint>
#include <expected>
#include <string>

namespace text
{

/// A font locator that never answers "no font" while the system has one.
///
/// A configured family that is not installed -- Cascadia Mono on a Windows without it, or a typo -- used to
/// leave the regular font unloadable, and the renderer gave up on the whole window. This locator degrades
/// instead, in order: the family asked for; then any monospace font (the generic `monospace` family, in the
/// weight and slant asked for, monospace preferred but not insisted on); then the first font the system
/// lists at all, which locators order fixed-pitch faces first. Each step down is logged.
///
/// Only locate() degrades; all(), resolve() and the coverage policy are the wrapped locator's.
class LastResortFontLocator final: public FontLocator
{
  public:
    /// @param inner The locator asked first, for everything. It must outlive this one.
    explicit LastResortFontLocator(FontLocator& inner);

    [[nodiscard]] CoverageFallbackSpacing coverageFallbackSpacing() const noexcept override;
    [[nodiscard]] FontSourceList all() override;
    [[nodiscard]] FontSourceList locate(FontDescription const& description) override;
    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> codepoints) override;

  private:
    FontLocator& _inner;
};

/// Why no font could be located for a description.
enum class FontUnavailable : std::uint8_t
{
    NoFontInstalled = 1, ///< Not the family asked for, nor any monospace font, nor any font at all.
};

/// Locates the font @p description will be drawn with: the first of @p locator's answers.
/// @param locator The locator to ask; a LastResortFontLocator answers whenever the system has any font.
/// @param description The configured font.
/// @return The font, or why there is none.
[[nodiscard]] std::expected<FontSource, FontUnavailable> locatePrimaryFont(
    FontLocator& locator, FontDescription const& description);

/// @param reason Why no font was located.
/// @param description The font that was asked for.
/// @return A message for the user: what is missing, and what to do about it.
[[nodiscard]] std::string describe(FontUnavailable reason, FontDescription const& description);

} // namespace text
