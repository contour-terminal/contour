// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.hpp>
#include <text_shaper/font_locator.hpp>

namespace text
{
class CoreTextLocator: public FontLocator
{
  public:
    CoreTextLocator();

    [[nodiscard]] FontSourceList locate(FontDescription const& description) override;
    [[nodiscard]] FontSourceList all() override;
    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> codepoints) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};
} // namespace text
