// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/FontFamilySource.hpp>

#include <QtCore/QStringList>
#include <QtGui/QFontDatabase>

#include <algorithm>
#include <ranges>

namespace contour::platform
{

std::vector<std::string> QtFontFamilySource::monospaceFamilies() const
{
    auto families = std::vector<std::string> {};
    for (auto const& family: QFontDatabase::families())
        if (QFontDatabase::isFixedPitch(family))
            families.push_back(family.toStdString());
    std::ranges::sort(families);
    families.erase(std::ranges::unique(families).begin(), families.end());
    std::erase(families, std::string { BundledMonospaceFamily });
    families.insert(families.begin(), std::string { BundledMonospaceFamily });
    return families;
}

} // namespace contour::platform
