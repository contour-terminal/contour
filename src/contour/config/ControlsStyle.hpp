// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace contour::config
{

/// Where a Qt Quick Controls style's qmldir lies beneath a QML import path, as QQuickStyle resolves the
/// style's name: a style of Qt's own (`QtQuick/Controls/<style>`), else a module of that name (ContourTui,
/// ContourLinux).
/// @param style The style's name.
/// @return The two paths, relative to an import path, with `/` separators.
[[nodiscard]] inline std::array<std::string, 2> controlsStyleQmldirs(std::string_view style)
{
    return { std::format("QtQuick/Controls/{}/qmldir", style), std::format("{}/qmldir", style) };
}

/// How a Qt Quick Controls style was arrived at.
enum class ControlsStyleOrigin : uint8_t
{
    Requested,         ///< The style asked for is installed.
    FellBackToDefault, ///< The style asked for is not installed, so the fallback style stands in for it.
};

/// The Qt Quick Controls style to pin, and how it was arrived at.
struct ControlsStyleChoice
{
    std::string style;          ///< The style to hand to QQuickStyle::setStyle().
    ControlsStyleOrigin origin; ///< Whether @c style is what was asked for.
};

/// Picks the style to pin: @p requested when @p isInstalled says it is there, else @p fallback.
///
/// A pinned style this Qt build does not ship (FluentWinUI3 on an older Qt, say) would otherwise fail the
/// whole QML load with `module "<style>" is not installed`.
/// @tparam Installed A callable taking the style's name and returning whether it is installed.
/// @param requested   The style asked for.
/// @param fallback    The style that is always there.
/// @param isInstalled Answers whether a style is installed; injected so the decision needs no filesystem.
/// @return The style to use and whether it is the one asked for.
template <typename Installed>
[[nodiscard]] ControlsStyleChoice chooseControlsStyle(std::string_view requested,
                                                      std::string_view fallback,
                                                      Installed const& isInstalled)
{
    if (isInstalled(requested))
        return { .style = std::string(requested), .origin = ControlsStyleOrigin::Requested };
    return { .style = std::string(fallback), .origin = ControlsStyleOrigin::FellBackToDefault };
}

} // namespace contour::config
