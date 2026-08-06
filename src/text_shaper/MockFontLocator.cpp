// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/MockFontLocator.hpp>

#include <text_shaper/Font.hpp>

#include <string_view>

using std::string;
using std::string_view;
using std::vector;

using namespace std::string_view_literals;

namespace text
{

namespace mockDetail
{
    static std::vector<FontDescriptionAndSource> registry;

    /// What resolve() answers with, regardless of the codepoints asked about.
    static FontSourceList coverage;
} // namespace mockDetail

void MockFontLocator::configure(std::vector<FontDescriptionAndSource> registry)
{
    mockDetail::registry = std::move(registry);
    mockDetail::coverage.clear();
}

void MockFontLocator::configureCoverage(FontSourceList sources)
{
    mockDetail::coverage = std::move(sources);
}

FontSourceList MockFontLocator::locate(FontDescription const& description)
{
    locatorLog()("Locating font chain for: {}", description);

    FontSourceList output;

    for (auto const& item: mockDetail::registry)
    {
        if (item.description != description)
            continue;

        output.emplace_back(item.source);
        break;
    }

    for (auto const& item: mockDetail::registry)
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

FontSourceList MockFontLocator::all()
{
    FontSourceList output;

    for (auto const& item: mockDetail::registry)
        output.emplace_back(item.source);

    return output;
}

FontSourceList MockFontLocator::resolve(gsl::span<char32_t const> /*codepoints*/)
{
    // A real locator answers by charset; a test says up front what the answer is, so that a case can
    // decide whether the coverage lookup finds anything without needing real fonts on the machine.
    return mockDetail::coverage;
}

} // namespace text
