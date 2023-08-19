// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>

namespace text
{

/**
 * Font locator API implementation using `fontconfig` library.
 *
 * This should be available on all platforms.
 *
 * @note on Windows, fontconfig still can NOT find user installed fonts.
 */
class fontconfig_locator: public font_locator
{
  public:
    fontconfig_locator();

    [[nodiscard]] font_source_list locate(font_description const& description) override;
    [[nodiscard]] font_source_list all() override;
    [[nodiscard]] font_source_list resolve(gsl::span<const char32_t> codepoints) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace text
