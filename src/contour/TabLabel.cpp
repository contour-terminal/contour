// SPDX-License-Identifier: Apache-2.0
#include <contour/TabLabel.h>

#include <crispy/interpolated_string.h>

#include <format>
#include <variant>

namespace contour
{

std::string expandTabLabel(std::string_view tmpl, TabLabelContext const& ctx)
{
    auto result = std::string {};

    for (auto const& fragment: crispy::parse_interpolated_string(tmpl))
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
        auto const& interpolation = std::get<crispy::string_interpolation>(fragment);
        if (interpolation.name == "WindowTitle")
            result += ctx.windowTitle;
        else if (interpolation.name == "TabPosition")
            result += std::format("{}", ctx.position);
        else
            result += interpolation.whole;
    }

    return result;
}

} // namespace contour
