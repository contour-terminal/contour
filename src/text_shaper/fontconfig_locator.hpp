// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.hpp>
#include <text_shaper/font_locator.hpp>

namespace text
{

/**
 * Font locator API implementation using `fontconfig` library.
 *
 * This should be available on all platforms.
 *
 * @note on Windows, fontconfig still can NOT find user installed fonts.
 */
class FontconfigLocator: public FontLocator
{
  public:
    FontconfigLocator();

    [[nodiscard]] FontSourceList locate(FontDescription const& description) override;
    [[nodiscard]] FontSourceList all() override;
    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> codepoints) override;

  private:
    struct PrivateTag;
    std::unique_ptr<PrivateTag, void (*)(PrivateTag*)> _d;
};

} // namespace text
