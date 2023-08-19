// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>

struct IDWriteFontFace;

namespace text
{

/**
 * Font locator API implementation using `DirectWrite` library.
 *
 * This is available only on Windows.
 */
class directwrite_locator: public font_locator
{
  public:
    directwrite_locator();

    font_source_list locate(font_description const& description) override;
    font_source_list all() override;
    font_source_list resolve(gsl::span<const char32_t> codepoints) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _d;
};

} // namespace text
