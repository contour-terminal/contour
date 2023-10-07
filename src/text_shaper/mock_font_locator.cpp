// SPDX-License-Identifier: Apache-2.0
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

font_source_list mock_font_locator::locate(font_description const& description)
{
    locatorLog()("Locating font chain for: {}", description);

    font_source_list output;

    for (auto const& item: mock_detail::registry)
    {
        if (item.description != description)
            continue;

        output.emplace_back(item.source);
        break;
    }

    for (auto const& item: mock_detail::registry)
    {
        if (item.description.slant != description.slant)
            continue;

        if (item.description.weight != description.weight)
            continue;

        if (item.description.spacing != description.spacing && description.strictSpacing)
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

font_source_list mock_font_locator::resolve(gsl::span<const char32_t> /*codepoints*/)
{
    return {};
}

} // namespace text
