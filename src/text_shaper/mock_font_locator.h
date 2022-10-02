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
    explicit mock_font_locator();

    [[nodiscard]] font_source_list locate(font_description const& description) override;
    [[nodiscard]] font_source_list all() override;
    [[nodiscard]] font_source_list resolve(gsl::span<const char32_t> codepoints) override;

    static void configure(std::vector<font_description_and_source> registry);
};

} // namespace text
