/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2021 Christian Parpart <christian@parpart.family>
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
    ~fontconfig_locator() override;

    font_source_list locate(font_description const& description) override;
    font_source_list all() override;
    font_source_list resolve(gsl::span<const char32_t> codepoints) override;

  private:
    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> d;
};

} // namespace text
