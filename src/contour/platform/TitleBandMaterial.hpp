// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's material behind a client-drawn title band, and the native title bar's measure.

#include <contour/config/ChromeTokenGroups.hpp>

#include <QtCore/Qt>
#include <QtCore/QtTypes>

#include <expected>
#include <memory>
#include <string_view>

class QWindow;

namespace contour::platform
{

/// Why the OS did not draw the material a title-band request asked for.
enum class TitleBandMaterialError : uint8_t
{
    Unsupported = 0, ///< The OS has no such backdrop.
    Rejected,        ///< The call failed on an OS that has it.
};

/// The name a log line gives @p error.
/// @param error Why the material was not drawn.
/// @return "unsupported" or "rejected".
[[nodiscard]] constexpr std::string_view titleBandMaterialErrorName(TitleBandMaterialError error) noexcept
{
    switch (error)
    {
        case TitleBandMaterialError::Unsupported: return "unsupported";
        case TitleBandMaterialError::Rejected: return "rejected";
    }
    return "rejected";
}

/// Whether a window's title-band material still works. Per-window state the controller keeps, not
/// configuration: once a material failed, that window paints the solid band for its lifetime (R-g).
enum class BandMaterialHealth : uint8_t
{
    Working = 0, ///< The material applied, or was never asked for.
    Failed,      ///< The material failed once: the window's band is Solid from then on.
};

/// What the controller asks the OS to draw behind and around a window's title band.
struct TitleBandRequest
{
    config::BandBackdrop backdrop = config::BandBackdrop::Client; ///< The OS's material, or nothing.
    /// Which window asks, which picks the material's variant (Mica Alt or Mica); macOS has one and ignores
    /// it.
    config::BackdropKind kind = config::BackdropKind::Tabbed;
    config::TitleBarGrowth growth =
        config::TitleBarGrowth::None;                  ///< Whether the title bar grows to the band.
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown; ///< The appearance; Unknown follows the app.

    [[nodiscard]] bool operator==(TitleBandRequest const&) const = default;
};

/// The native title bar as measured, in logical pixels; all zero where there is none.
struct NativeTitleBarMetrics
{
    /// The title bar's height. On macOS it is Qt's safe-area top, QWindow::safeAreaMargins().top(), which
    /// Qt::ExpandedClientAreaHint makes the title bar's extent above the content layout rectangle. In full
    /// screen it is the title bar AppKit reveals over the window's top (a bare one: full screen detaches the
    /// compact toolbar, ruling S7), whose window takes the clicks over it -- so the band rests below it.
    qreal height = 0.0;
    /// In full screen, where a revealed title bar's top sits below the window's top; window-relative, >= 0.
    qreal fullScreenTop = 0.0;

    [[nodiscard]] bool operator==(NativeTitleBarMetrics const&) const = default;
};

/// Draws the operating system's material behind a client-drawn title band, and measures its title bar.
class ITitleBandMaterial
{
  public:
    virtual ~ITitleBandMaterial() = default;

    /// Applies @p request to @p window. Best-effort: a window with no platform window yet is left alone,
    /// and succeeds.
    /// @pre GUI (main) thread.
    /// @param window The window whose title bar to dress.
    /// @param request What to draw behind and around its band.
    /// @return Nothing on success; why the OS did not draw the requested backdrop otherwise.
    [[nodiscard]] virtual std::expected<void, TitleBandMaterialError> apply(
        QWindow& window, TitleBandRequest const& request) = 0;

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
    /// @return Success: there is nothing to fail.
    [[nodiscard]] std::expected<void, TitleBandMaterialError> apply(
        QWindow& /*window*/, TitleBandRequest const& /*request*/) override
    {
        return {};
    }

    /// @return Zeros: there is no native title bar to measure.
    [[nodiscard]] NativeTitleBarMetrics metrics(QWindow& /*window*/) const override { return {}; }
};

/// Creates the operating system's title-band material, or the null one where there is none. Built before
/// a QGuiApplication exists, so a native material checks the platform plugin when it is called, not here.
/// @return The material.
[[nodiscard]] std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial();

} // namespace contour::platform
