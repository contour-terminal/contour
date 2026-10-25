// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabLabel.hpp>

#include <crispy/InterpolatedString.hpp>

#include <format>
#include <utility>
#include <variant>

namespace contour::window
{

std::string expandTabLabel(std::string_view tmpl, TabLabelContext const& ctx)
{
    auto result = std::string {};

    for (auto const& fragment: crispy::parseInterpolatedString(tmpl))
    {
        if (std::holds_alternative<std::string_view>(fragment))
        {
            result += std::get<std::string_view>(fragment);
            continue;
        }

        // A `{Name:flags,key=value}` placeholder. Tab labels are plain text, so flags/attributes are
        // ignored; only the name selects a value. An unrecognized name is echoed verbatim (its exact
        // original `{...}` slice), matching parseStatusLineSegment's handling so both surfaces treat an
        // unknown placeholder the same way — the user sees what they typed rather than it vanishing.
        auto const& interpolation = std::get<crispy::StringInterpolation>(fragment);
        if (interpolation.name == "WindowTitle")
            result += ctx.windowTitle;
        else if (interpolation.name == "TabPosition")
            result += std::format("{}", ctx.position);
        else
            result += interpolation.whole;
    }

    return result;
}

std::string tabLabelWithFallback(std::string expanded, std::string_view processName)
{
    return expanded.empty() ? std::string { processName } : std::move(expanded);
}

} // namespace contour::window
