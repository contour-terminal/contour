// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/WindowControlStyle.hpp>

#include <QtCore/QObject>
#include <QtCore/QVariantList>
#include <QtGui/QColor>

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

    /// Which end of the title bar the controls sit at: "leading" or "trailing".
    ///
    /// A string rather than the enumerator's underlying value, for the same reason each button's
    /// `action` is one: an enum reaches QML only once registered with the meta-object system, and a
    /// context property has no type name for QML to reach the enumerators through -- so the
    /// alternative is a bare `=== 1` in the QML whose meaning lives in this header, and which
    /// reordering @c config::WindowControlSide would silently invert.
    Q_PROPERTY(QString side READ side NOTIFY changed)

    /// The shape the controls are drawn as: "button" or "trafficLight". @see side.
    Q_PROPERTY(QString presentation READ presentation NOTIFY changed)

    /// The controls, in draw order. @see buttons().
    Q_PROPERTY(QVariantList buttons READ buttons NOTIFY changed)

    Q_PROPERTY(int hoverCornerPercent READ hoverCornerPercent NOTIFY changed)

    /// What every dot fades to while the window is inactive, as "#rrggbb", or empty in a style that
    /// draws no dots. A string rather than a QColor so that "unset" survives the trip into QML,
    /// whose color value type has no validity to test. @see the buttons() rows.
    Q_PROPERTY(QString inactiveDotColor READ inactiveDotColor NOTIFY changed)

  public:
    /// @param resolved A concrete style, i.e. the result of @c config::resolveWindowControlStyle.
    ///                 @c Auto is accepted but resolves to the historical Windows appearance rather
    ///                 than to the host's -- resolving is the caller's job, because only the
    ///                 composition root knows the environment to resolve against.
    /// @param parent   Qt ownership.
    explicit WindowControlStyleProvider(config::WindowControlStyle resolved, QObject* parent = nullptr);

    /// Re-points this at @p resolved.
    ///
    /// The documented exception to the complete-constructor rule, and live reconfiguration is
    /// precisely the feature: a settings-page change must move the controls without a restart. Unlike
    /// @c Renderer::setFonts it costs nothing to offer -- there is no staged state and no lock,
    /// because everything here is a pure function of one enumerator.
    ///
    /// @param resolved A concrete style, as for the constructor.
    void setStyle(config::WindowControlStyle resolved);

    [[nodiscard]] QString side() const;
    [[nodiscard]] QString presentation() const;
    [[nodiscard]] QVariantList buttons() const;
    [[nodiscard]] int hoverCornerPercent() const noexcept;
    [[nodiscard]] QString inactiveDotColor() const;

  signals:
    /// Emitted when the style actually changed, so every binding above re-evaluates.
    ///
    /// One signal for the whole object rather than one per property: they all change together or
    /// not at all, because they are read from a single table row.
    void changed();

  private:
    config::WindowControlStyle _style;
};

} // namespace contour::window
