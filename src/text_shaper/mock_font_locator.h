// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>

namespace text
{

struct font_description_and_source
{
    font_description description;
    font_source source;
};

/**
 * Font locator API implementation that requires
 * manual configuration.
 *
 * This should be available on all platforms.
 */
class mock_font_locator: public font_locator
{
  public:
    [[nodiscard]] font_source_list locate(font_description const& description) override;
    [[nodiscard]] font_source_list all() override;
    [[nodiscard]] font_source_list resolve(gsl::span<char32_t const> codepoints) override;

    static void configure(std::vector<font_description_and_source> registry);

    /// Sets what resolve() answers with. Cleared by configure(), so a case cannot leak into the next.
    static void configureCoverage(font_source_list sources);
};

} // namespace text
