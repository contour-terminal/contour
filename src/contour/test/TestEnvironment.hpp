// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>

#include <string>
#include <string_view>
#include <utility>

/// The run-wide knobs the GUI test runner reads from its environment (see test_main.cpp).
namespace contour::test
{

/// The variable that opts a run into the display-gated cases.
inline constexpr std::string_view DisplayTestsVariable = "CONTOUR_TEST_DISPLAY";

/// Whether this run renders through a real windowing system (see test_main.cpp).
/// @param env The environment to read.
/// @return True when CONTOUR_TEST_DISPLAY is `1`.
[[nodiscard]] inline bool displayTestsEnabled(core::Environment const& env)
{
    return env.get(DisplayTestsVariable) == "1";
}

/// The Quick Controls style a run uses unless the environment says otherwise.
inline constexpr std::string_view DefaultControlsStyle = "Fusion";
/// The variable that selects the Quick Controls style for the whole run.
inline constexpr std::string_view ControlsStyleVariable = "CONTOUR_TEST_CONTROLS_STYLE";

/// The Quick Controls style to run under.
/// @param env The environment to read.
/// @return The variable's value, or @c DefaultControlsStyle when it is unset or empty.
[[nodiscard]] inline std::string controlsStyleFor(core::Environment const& env)
{
    if (auto value = env.get(ControlsStyleVariable); value && !value->empty())
        return *std::move(value);
    return std::string { DefaultControlsStyle };
}

} // namespace contour::test
