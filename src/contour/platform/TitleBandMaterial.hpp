// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's material behind a client-drawn title band, and the native title bar's measure.

#include <contour/config/ChromeTokenGroups.hpp>

#include <QtCore/Qt>
#include <QtCore/QtTypes>

#include <memory>

class QWindow;

namespace contour::platform
{

/// What the controller asks the OS to draw behind and around a window's title band.
struct TitleBandRequest
{
    config::BandBackdrop backdrop = config::BandBackdrop::Client; ///< The OS's material, or nothing.
    config::TitleBarGrowth growth =
        config::TitleBarGrowth::None;                  ///< Whether the title bar grows to the band.
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown; ///< The appearance; Unknown follows the app.

    [[nodiscard]] bool operator==(TitleBandRequest const&) const = default;
};

/// The native title bar as measured, in logical pixels; all zero where there is none.
struct NativeTitleBarMetrics
{
    qreal height = 0.0; ///< The title bar's height above the content layout rectangle.
    /// In full screen, where a revealed title bar's top sits below the window's top; window-relative, >= 0.
    qreal fullScreenTop = 0.0;

    [[nodiscard]] bool operator==(NativeTitleBarMetrics const&) const = default;
};

/// Draws the operating system's material behind a client-drawn title band, and measures its title bar.
class ITitleBandMaterial
{
  public:
    virtual ~ITitleBandMaterial() = default;

    /// Applies @p request to @p window. Best-effort: a window with no platform window yet is left alone.
    /// @pre GUI (main) thread.
    /// @param window The window whose title bar to dress.
    /// @param request What to draw behind and around its band.
    virtual void apply(QWindow& window, TitleBandRequest const& request) = 0;

    /// @pre GUI (main) thread.
    /// @param window The window to measure.
    /// @return @p window's native title bar; zeros where there is none.
    [[nodiscard]] virtual NativeTitleBarMetrics metrics(QWindow& window) const = 0;
};

/// The material for platforms that draw none: apply() does nothing and metrics() measures nothing.
class NullTitleBandMaterial final: public ITitleBandMaterial
{
  public:
    /// Does nothing.
    void apply(QWindow& /*window*/, TitleBandRequest const& /*request*/) override {}

    /// @return Zeros: there is no native title bar to measure.
    [[nodiscard]] NativeTitleBarMetrics metrics(QWindow& /*window*/) const override { return {}; }
};

/// Creates the operating system's title-band material, or the null one where there is none. Built before
/// a QGuiApplication exists, so a native material checks the platform plugin when it is called, not here.
/// @return The material.
[[nodiscard]] std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial();

} // namespace contour::platform
