// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/Font.hpp>
#include <text_shaper/FontLocator.hpp>

namespace text
{
class CoreTextLocator: public FontLocator
{
  public:
    /// Creates a native locator with a thread-safe, file-identity-checked collection cache.
    CoreTextLocator();

    /// CoreText's final coverage substitute may be proportional (e.g. mathematical symbols).
    /// Preserve the terminal grid while accepting that native substitute, as DirectWrite does.
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
