// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>

#include <optional>
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

/// The variable Qt's FreeType font database reads its font directory from.
inline constexpr std::string_view QtFontDirectoryVariable = "QT_QPA_FONTDIR";

/// The font directory to give the offscreen platform on Windows.
///
/// Offscreen, Qt reads fonts not from the system but from one directory: QT_QPA_FONTDIR, else Qt's own
/// `lib/fonts`, which a Qt installation does not ship. Without it the run has no font at all, and every
/// glyph is drawn -- and measured -- as a box a whole em wide: a cell-counting chrome comes out two-thirds
/// wider than on any real machine. The system's fonts directory is what the native platform would use.
/// @param env The environment to read.
/// @return `<WINDIR>/Fonts`; or std::nullopt when QT_QPA_FONTDIR is already set, or WINDIR is not.
[[nodiscard]] inline std::optional<std::string> offscreenFontDirectory(core::Environment const& env)
{
    if (auto const configured = env.get(QtFontDirectoryVariable); configured && !configured->empty())
        return std::nullopt;
    auto const windows = env.get("WINDIR");
    if (!windows || windows->empty())
        return std::nullopt;
    return *windows + "/Fonts";
}

} // namespace contour::test
