// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/GpuSelection.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::display
{

/// One environment variable to set.
struct EnvironmentAssignment
{
    std::string name;  ///< Variable name.
    std::string value; ///< Variable value.

    [[nodiscard]] bool operator==(EnvironmentAssignment const&) const = default;
};

namespace detail
{
    /// Kernel drivers whose OpenGL stack is selected by something other than Mesa's DRI_PRIME.
    struct DriverOffload
    {
        std::string_view driver;
        std::array<std::pair<std::string_view, std::string_view>, 2> variables;
    };

    inline constexpr auto DriverOffloadTable = std::array {
        DriverOffload { .driver = "nvidia",
                        .variables = { { { "__NV_PRIME_RENDER_OFFLOAD", "1" },
                                         { "__GLX_VENDOR_LIBRARY_NAME", "nvidia" } } } },
    };
} // namespace detail

/// The variables that make OpenGL render on @p chosen. Qt has no OpenGL device API: the driver stack
/// reads these when the display connection opens, so they must be set before QGuiApplication exists.
/// @param chosen The GPU to render on.
/// @param isAlreadySet Whether a variable is already in the environment (the user's choice wins).
/// @return Nothing when @p chosen drives the display or the user already chose; else the variables.
[[nodiscard]] inline std::vector<EnvironmentAssignment> openGlSelectionEnvironment(
    GpuCandidate const& chosen, std::function<bool(std::string_view)> const& isAlreadySet)
{
    if (chosen.output == GpuOutput::DrivesDisplay)
        return {};

    auto assignments = std::vector<EnvironmentAssignment> {};
    auto const offload =
        std::ranges::find(detail::DriverOffloadTable, chosen.driver, &detail::DriverOffload::driver);
    if (offload != detail::DriverOffloadTable.end())
        for (auto const& [name, value]: offload->variables)
            assignments.push_back({ .name = std::string(name), .value = std::string(value) });
    else
        assignments.push_back({ .name = "DRI_PRIME", .value = std::format("{}", chosen.id) });

    if (std::ranges::any_of(assignments, [&](auto const& a) { return isAlreadySet(a.name); }))
        return {};
    return assignments;
}

} // namespace contour::display
