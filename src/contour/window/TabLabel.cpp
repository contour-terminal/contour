// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabLabel.hpp>

#include <crispy/InterpolatedString.hpp>

#include <format>
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
        else if (interpolation.name == "TabName")
            result += ctx.tabName;
        else if (interpolation.name == "TabPosition")
            result += std::format("{}", ctx.position);
        else
            result += interpolation.whole;
    }

    return result;
}

std::string blankTabLabelFallback(std::string_view foregroundProcess, std::string_view shellProgram)
{
    if (!foregroundProcess.empty())
        return std::string { foregroundProcess };

    // The program is its first word (the rest are its arguments), then the part after the last separator.
    auto program = shellProgram.substr(0, shellProgram.find(' '));
    if (auto const separator = program.find_last_of("/\\"); separator != std::string_view::npos)
        program.remove_prefix(separator + 1);
    if (program.starts_with('-'))
        program.remove_prefix(1);
    return std::string { program };
}

} // namespace contour::window
