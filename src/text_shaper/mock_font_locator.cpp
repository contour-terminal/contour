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
#include <text_shaper/font.h>
#include <text_shaper/mock_font_locator.h>

#include <string_view>

using std::nullopt;
using std::optional;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

using namespace std::string_view_literals;

namespace text
{

namespace mock_detail
{
    static std::vector<font_description_and_source> registry;
}

void mock_font_locator::configure(std::vector<font_description_and_source> registry)
{
    mock_detail::registry = std::move(registry);
}

mock_font_locator::mock_font_locator()
{
}

font_source_list mock_font_locator::locate(font_description const& _fd)
{
    LOGSTORE(LocatorLog)("Locating font chain for: {}", _fd);

    font_source_list output;

    auto const addFontFile = [&](std::string_view path) {
        output.emplace_back(font_path { string { path } });
    };

    for (auto const& item: mock_detail::registry)
    {
        if (item.description != _fd)
            continue;

        output.emplace_back(item.source);
        break;
    }

    for (auto const& item: mock_detail::registry)
    {
        if (item.description.slant != _fd.slant)
            continue;

        if (item.description.weight != _fd.weight)
            continue;

        if (item.description.spacing != _fd.spacing && _fd.strict_spacing)
            continue;

        output.emplace_back(item.source);
    }

    return output;
}

font_source_list mock_font_locator::all()
{
    font_source_list output;

    for (auto const& item: mock_detail::registry)
        output.emplace_back(item.source);

    return output;
}

font_source_list mock_font_locator::resolve(gsl::span<const char32_t> codepoints)
{
    return {};
}

} // namespace text
