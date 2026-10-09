// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/GpuSelection.hpp>

#include <vtpty/EnvironmentBlock.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <ranges>
#include <span>
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

/// Name of the variable that tells a Contour started by another Contour which variables its parent set
/// to choose a GPU (as opposed to ones the user exported). Its value is a comma-separated list of names.
inline constexpr std::string_view SelfSetEnvironmentMarker = "CONTOUR_SELF_SET_ENVIRONMENT";

/// Parses the value of #SelfSetEnvironmentMarker.
/// @param markerValue Comma-separated variable names.
/// @return The names, in order, with surrounding whitespace removed and empty entries dropped.
[[nodiscard]] inline std::vector<std::string> selfSetNamesFrom(std::string_view markerValue)
{
    auto names = std::vector<std::string> {};
    for (auto const part: core::split(markerValue, ','))
        if (auto const name = core::trim(part); !name.empty())
            names.emplace_back(name);
    return names;
}

/// Builds the value of #SelfSetEnvironmentMarker.
/// @param assignments The variables Contour set to choose its GPU.
/// @return Their names, comma-separated; empty when there are none.
[[nodiscard]] inline std::string selfSetMarkerValue(std::span<EnvironmentAssignment const> assignments)
{
    auto value = std::string {};
    for (auto const& assignment: assignments)
    {
        if (!value.empty())
            value += ',';
        value += assignment.name;
    }
    return value;
}

/// Name of the variable through which a Contour whose OpenGL GPU could not render restarts itself on
/// the automatic GPU. Its value is the failed GPU's title; the restarted Contour reports the fallback,
/// uses `auto` for that run and removes the variable, so it is never inherited further.
inline constexpr std::string_view GpuFallbackEnvironmentName = "CONTOUR_GPU_FALLBACK";

/// The environment for restarting Contour on the automatic GPU, after the OpenGL GPU chosen through the
/// driver's variables could not render.
/// @param inherited This process's environment, as "NAME=VALUE" entries.
/// @param selfSet The variables this Contour set to choose its GPU, the marker included.
/// @param failedTitle The title of the GPU that could not render.
/// @return @p inherited without @p selfSet, plus #GpuFallbackEnvironmentName naming @p failedTitle.
[[nodiscard]] inline std::vector<std::string> gpuFallbackRelaunchEnvironment(
    std::span<std::string_view const> inherited,
    std::span<std::string const> selfSet,
    std::string_view failedTitle)
{
    return vtpty::buildEnvironmentBlock(
        inherited,
        std::map<std::string, std::string> {
            { std::string(GpuFallbackEnvironmentName), std::string(failedTitle) } },
        selfSet);
}

/// The title of the GPU the system renders on when Contour does not intervene: the one driving the
/// display.
/// @param gpus The machine's GPUs.
/// @return That GPU's title, or a generic phrase when no GPU says it drives the display.
[[nodiscard]] inline std::string displayGpuTitle(std::span<GpuCandidate const> gpus)
{
    auto const display = std::ranges::find(gpus, GpuOutput::DrivesDisplay, &GpuCandidate::output);
    return display != gpus.end() ? display->title : std::string { "the system's default GPU" };
}

} // namespace contour::display
