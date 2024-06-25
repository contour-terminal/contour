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

    font_locator& native();

    font_locator& mock();

  private:
    std::unique_ptr<font_locator> _native {};
    std::unique_ptr<font_locator> _mock {};
};

} // namespace text
