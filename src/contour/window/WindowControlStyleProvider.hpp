// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/DesktopButtonLayout.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/PlatformAppearance.hpp>

#include <QtCore/QObject>
#include <QtCore/QVariantList>

#include <functional>
#include <span>
#include <string_view>

namespace contour::window
{

/// What the window controls are and where they go, published to QML as the @c windowControls
/// context property.
///
/// Deliberately separate from @c UiStyleProvider, and the split is the design: this says *what and
/// where* -- which side, which order, which shape, which colors -- while @c chromeStyle says *how
/// big*, in whichever unit the active @c UiStyle counts in. Because no extent appears here, every
/// window-control style composes with either chrome style for free, instead of the two tables
/// multiplying out into a style per combination.
///
/// The other reason it is separate is lifetime. Every @c UiStyleProvider property is CONSTANT by
/// contract -- the Quick Controls half of a @c UiStyle is chosen before the first control exists, so
/// a live-updating geometry half would only ever half-switch the window. Nothing here is pinned that
/// way: which corner the buttons sit in is decided every frame by a binding, so this one CAN change
/// live, and does.
///
/// Like @c UiStyleProvider this is a context property rather than a registered QML singleton, so
/// each test engine can carry its own (see test/QmlChromeStyle.hpp). The same price applies:
/// a context property is untyped, so qmlcachegen cannot ahead-of-time compile bindings that read it.
class WindowControlStyleProvider: public QObject
{
    Q_OBJECT

    /// The shape the controls are drawn as: "button", "trafficLight" or "round" (config::presentationFor, so
    /// every style draws dots on the Terminal row).
    ///
    /// A string rather than the enumerator's underlying value, for the same reason each button's
    /// `action` is one: an enum reaches QML only once registered with the meta-object system, and a
    /// context property has no type name for QML to reach the enumerators through -- so the
    /// alternative is a bare `=== 1` in the QML whose meaning lives in this header, and which
    /// reordering @c config::WindowControlPresentation would silently invert.
    Q_PROPERTY(QString presentation READ presentation NOTIFY changed)

    /// The controls before the bar's content, in draw order (leading edge first). @see buttonsOf().
    Q_PROPERTY(QVariantList leadingButtons READ leadingButtons NOTIFY changed)

    /// The controls after the bar's content, in draw order (leading edge first). @see buttonsOf().
    Q_PROPERTY(QVariantList trailingButtons READ trailingButtons NOTIFY changed)

    Q_PROPERTY(int hoverCornerPercent READ hoverCornerPercent NOTIFY changed)

    /// What every dot fades to while the window is inactive, as "#rrggbb", or empty in a style that
    /// draws no dots. A string rather than a QColor so that "unset" survives the trip into QML,
    /// whose color value type has no validity to test. @see the buttons() rows.
    Q_PROPERTY(QString inactiveDotColor READ inactiveDotColor NOTIFY changed)

  public:
    /// @param resolved        A concrete style, i.e. the result of @c config::resolveWindowControlStyle.
    ///                        @c Auto is accepted but resolves to the historical Windows appearance rather
    ///                        than to the host's -- resolving is the caller's job, because only the
    ///                        composition root knows the environment to resolve against.
    /// @param placement       Which controls go on which side, in which order
    ///                        (config::windowButtonPlacement of @p resolved and the desktop's layout).
    /// @param row             The chrome row the controls sit on, fixed for the provider's life as the
    ///                        chrome's own row is.
    /// @param contrast        Whether the OS runs a contrast theme now. @see setContrastTheme.
    /// @param isFontInstalled Whether a font family is installed (QFontDatabase::hasFamily in production),
    /// for
    ///                        the style's icon font (config::controlGlyphsFor).
    /// @param parent          Qt ownership.
    WindowControlStyleProvider(config::WindowControlStyle resolved,
                               config::WindowButtonLayout placement,
                               config::ChromeStyle row,
                               platform::ContrastTheme contrast,
                               std::function<bool(std::string_view)> isFontInstalled,
                               QObject* parent = nullptr);

    /// Re-points this at @p resolved, placed by @p placement; emits @c changed only when either differs.
    ///
    /// The documented exception to the complete-constructor rule, and live reconfiguration is
    /// precisely the feature: a settings-page change, or the desktop moving its buttons, must move the
    /// controls without a restart. Unlike @c Renderer::setFonts it costs nothing to offer -- there is no
    /// staged state and no lock, because everything here is a pure function of these two values.
    ///
    /// @param resolved  A concrete style, as for the constructor.
    /// @param placement Which controls go on which side, as for the constructor.
    void update(config::WindowControlStyle resolved, config::WindowButtonLayout placement);

    /// Follows the OS's contrast theme: under @c ContrastTheme::Active no button keeps a hover fill of its
    /// own, so the close button washes with the palette like its neighbours instead of turning red.
    ///
    /// Live OS state, not configuration: the user switches a contrast theme on and off while the window is
    /// open, and the provider is the one object every window's controls read.
    ///
    /// @param contrast Whether the OS runs a contrast theme now.
    void setContrastTheme(platform::ContrastTheme contrast);

    [[nodiscard]] QString presentation() const;
    /// @return The rows of the placement's leading side.
    [[nodiscard]] QVariantList leadingButtons() const;
    /// @return The rows of the placement's trailing side.
    [[nodiscard]] QVariantList trailingButtons() const;
    [[nodiscard]] int hoverCornerPercent() const noexcept;
    [[nodiscard]] QString inactiveDotColor() const;

    /// The rows for @p kinds, in their order, whichever side the desktop's layout puts each on -- or none:
    /// what a window that picks its own controls draws (the Settings window's close, settingsWindowControls).
    /// @param kinds config::WindowControlKind values; anything else is skipped.
    /// @return The rows, as leadingButtons' are.
    [[nodiscard]] Q_INVOKABLE QVariantList buttonsFor(QVariantList const& kinds) const;

  signals:
    /// Emitted when the style actually changed, so every binding above re-evaluates.
    ///
    /// One signal for the whole object rather than one per property: they all change together or
    /// not at all, because they are read from a single table row.
    void changed();

  private:
    /// The rows QML iterates for @p kinds, in their order.
    [[nodiscard]] QVariantList buttonsOf(std::span<config::WindowControlKind const> kinds) const;

    config::WindowControlStyle _style;
    config::WindowButtonLayout _placement;
    config::ChromeStyle _row;
    platform::ContrastTheme _contrast;
    std::function<bool(std::string_view)> _isFontInstalled;
};

} // namespace contour::window
