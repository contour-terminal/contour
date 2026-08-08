// SPDX-License-Identifier: Apache-2.0
#include <contour/window/WindowControlStyleProvider.hpp>

#include <QtCore/QVariantMap>

#include <cstddef>

namespace contour::window
{

namespace
{
    /// What QML calls @p kind, and what it dispatches on to reach the window controller.
    ///
    /// Strings rather than the enumerator's own value, so the QML reads as what it does
    /// (`activate("minimize")`) instead of as a number whose meaning lives in a header. They are also
    /// what QML keys its translated accessible names off, which is why those stay in the QML.
    [[nodiscard]] QString actionOf(config::WindowControlKind kind)
    {
        switch (kind)
        {
            case config::WindowControlKind::Close: return QStringLiteral("close");
            case config::WindowControlKind::Minimize: return QStringLiteral("minimize");
            case config::WindowControlKind::Maximize: return QStringLiteral("maximize");
        }
        return {};
    }

    /// The glyph @p kind is drawn with, per @p tokens.
    [[nodiscard]] QString glyphOf(config::WindowControlTokens const& tokens, config::WindowControlKind kind)
    {
        return QString::fromUtf8(tokens.glyphs[static_cast<size_t>(kind)]);
    }

    /// @p rgb as the "#rrggbb" string QML assigns to a color property, or an EMPTY string when it
    /// is 0.
    ///
    /// A string rather than a QColor because "unset" has to survive the trip: QML's `color` value
    /// type has no validity to test, so an invalid QColor arrives indistinguishable from black,
    /// whereas an empty string is falsy and reads as `button.hoverFill !== ""` at the use site. What
    /// a style leaves unset is "you decide" -- the chrome's own hover wash -- not "paint nothing".
    [[nodiscard]] QString colorOrUnset(uint32_t rgb)
    {
        return rgb != 0 ? QColor::fromRgb(rgb).name() : QString {};
    }
} // namespace

WindowControlStyleProvider::WindowControlStyleProvider(config::WindowControlStyle resolved, QObject* parent):
    QObject { parent }, _style { resolved }
{
}

void WindowControlStyleProvider::setStyle(config::WindowControlStyle resolved)
{
    if (_style == resolved)
        return;

    _style = resolved;
    emit changed();
}

QString WindowControlStyleProvider::side() const
{
    switch (config::windowControlTokens(_style).side)
    {
        case config::WindowControlSide::Trailing: return QStringLiteral("trailing");
        case config::WindowControlSide::Leading: return QStringLiteral("leading");
    }
    return {};
}

QString WindowControlStyleProvider::presentation() const
{
    switch (config::windowControlTokens(_style).presentation)
    {
        case config::WindowControlPresentation::Button: return QStringLiteral("button");
        case config::WindowControlPresentation::TrafficLight: return QStringLiteral("trafficLight");
    }
    return {};
}

int WindowControlStyleProvider::hoverCornerPercent() const noexcept
{
    return config::windowControlTokens(_style).hoverCornerPercent;
}

QString WindowControlStyleProvider::inactiveDotColor() const
{
    return colorOrUnset(config::windowControlTokens(_style).inactiveDotColor);
}

QVariantList WindowControlStyleProvider::buttons() const
{
    // One list of fully-resolved rows rather than a handful of parallel accessors QML would have to
    // index in step: the QML iterates this with a Repeater and reads each row's own fields, so
    // adding a control -- or a style that orders them differently -- changes no QML at all.
    auto const& tokens = config::windowControlTokens(_style);
    auto rows = QVariantList {};

    for (auto const kind: tokens.order)
    {
        auto const glyph = glyphOf(tokens, kind);

        auto row = QVariantMap {};
        row[QStringLiteral("action")] = actionOf(kind);
        row[QStringLiteral("glyph")] = glyph;
        // Only maximize has a second state, but every row carries the field so the delegate can read
        // it unconditionally; for the others it simply equals `glyph`.
        row[QStringLiteral("restoreGlyph")] =
            kind == config::WindowControlKind::Maximize ? QString::fromUtf8(tokens.restoreGlyph) : glyph;
        row[QStringLiteral("dotColor")] = colorOrUnset(tokens.dotColors[static_cast<size_t>(kind)]);
        // The close button is the only one any style tints on hover, and it is the only one whose
        // hover says something the palette cannot -- so the field is per row rather than per style.
        row[QStringLiteral("hoverFill")] =
            kind == config::WindowControlKind::Close ? colorOrUnset(tokens.closeHoverColor) : QString {};
        rows.push_back(row);
    }

    return rows;
}

} // namespace contour::window
