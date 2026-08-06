// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.hpp>
#include <text_shaper/font_locator.hpp>

namespace text
{

struct FontDescriptionAndSource
{
    FontDescription description;
    FontSource source;
};

/**
 * Font locator API implementation that requires
 * manual configuration.
 *
 * This should be available on all platforms.
 */
class MockFontLocator: public FontLocator
{
  public:
    [[nodiscard]] FontSourceList locate(FontDescription const& description) override;
    [[nodiscard]] FontSourceList all() override;
    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> codepoints) override;

    static void configure(std::vector<FontDescriptionAndSource> registry);

    /// Sets what resolve() answers with. Cleared by configure(), so a case cannot leak into the next.
    static void configureCoverage(FontSourceList sources);
};

} // namespace text
