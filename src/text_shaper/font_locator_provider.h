// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/font_locator.h>

#include <memory>

namespace text
{

class font_locator_provider
{
  public:
    static font_locator_provider& get();

#if defined(__APPLE__)
    font_locator& coretext();
#endif

#if defined(_WIN32)
    font_locator& directwrite();
#endif

    font_locator& fontconfig();
    font_locator& mock();

  private:
#if defined(__APPLE__)
    std::unique_ptr<font_locator> _coretext {};
#endif

#if defined(_WIN32)
    std::unique_ptr<font_locator> _directwrite {};
#endif

    std::unique_ptr<font_locator> _fontconfig {};
    std::unique_ptr<font_locator> _mock {};
};

} // namespace text
