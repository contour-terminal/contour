/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
