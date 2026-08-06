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

    FontSourceList locate(FontDescription const& description) override;
    FontSourceList all() override;
    FontSourceList resolve(gsl::span<char32_t const> codepoints) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace text
