// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/Font.hpp>
#include <text_shaper/FontLocator.hpp>

struct IDWriteFontFace;

namespace text
{

/**
 * Font locator API implementation using `DirectWrite` library.
 *
 * This is available only on Windows.
 */
class DirectWriteLocator: public FontLocator
{
  public:
    DirectWriteLocator();

    /// Symbols absent from monospace fonts (U+23F5 lives only in Segoe UI Symbol) need a proportional
    /// system face, and a real glyph beats the replacement character. This only governs the
    /// last-resort lookup; the configured chain still follows `FontDescription::strictSpacing`.
    [[nodiscard]] CoverageFallbackSpacing coverageFallbackSpacing() const noexcept override
    {
        return CoverageFallbackSpacing::Unrestricted;
    }

    [[nodiscard]] FontSourceList locate(FontDescription const& description) override;
    [[nodiscard]] FontSourceList all() override;
    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> codepoints) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace text
