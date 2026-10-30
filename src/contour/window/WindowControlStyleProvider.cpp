// SPDX-License-Identifier: Apache-2.0
#include <contour/config/WindowControlLook.hpp>
#include <contour/window/WindowControlStyleProvider.hpp>

#include <QtCore/QVariantMap>
#include <QtGui/QColor>

#include <cstddef>
#include <utility>
#include <vector>

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

    /// The glyph @p kind is drawn with, per @p glyphs.
    [[nodiscard]] QString glyphOf(config::ControlGlyphs const& glyphs, config::WindowControlKind kind)
    {
        return QString::fromUtf8(glyphs.glyphs[static_cast<size_t>(kind)]);
    }

    /// What QML calls @p wash: a string for the reason `action` is one.
    [[nodiscard]] QString washOf(config::ControlWash wash)
    {
        switch (wash)
        {
            case config::ControlWash::Highlight: return QStringLiteral("highlight");
            case config::ControlWash::KitFill: return QStringLiteral("kitFill");
        }
        return {};
    }

    /// The bundled copy of the freedesktop icon @p iconName (resources.qrc's /contour/icons), or empty for no
    /// icon.
    [[nodiscard]] QString bundledIconOf(QString const& iconName)
    {
        return iconName.isEmpty() ? QString {} : QStringLiteral("qrc:/contour/icons/%1.svg").arg(iconName);
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

WindowControlStyleProvider::WindowControlStyleProvider(config::WindowControlStyle resolved,
                                                       config::WindowButtonLayout placement,
                                                       config::ChromeStyle row,
                                                       platform::ContrastTheme contrast,
                                                       std::function<bool(std::string_view)> isFontInstalled,
                                                       QObject* parent):
    QObject { parent },
    _style { resolved },
    _placement { std::move(placement) },
    _row { row },
    _contrast { contrast },
    _isFontInstalled { std::move(isFontInstalled) }
{
}

void WindowControlStyleProvider::setContrastTheme(platform::ContrastTheme contrast)
{
    if (_contrast == contrast)
        return;

    _contrast = contrast;
    emit changed();
}

void WindowControlStyleProvider::update(config::WindowControlStyle resolved,
                                        config::WindowButtonLayout placement)
{
    if (_style == resolved && _placement == placement)
        return;

    _style = resolved;
    _placement = std::move(placement);
    emit changed();
}

QString WindowControlStyleProvider::presentation() const
{
    switch (config::presentationFor(config::windowControlTokens(_style), _row))
    {
        case config::WindowControlPresentation::Button: return QStringLiteral("button");
        case config::WindowControlPresentation::TrafficLight: return QStringLiteral("trafficLight");
        case config::WindowControlPresentation::Round: return QStringLiteral("round");
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

QVariantList WindowControlStyleProvider::leadingButtons() const
{
    return buttonsOf(_placement.leading);
}

QVariantList WindowControlStyleProvider::trailingButtons() const
{
    return buttonsOf(_placement.trailing);
}

QVariantList WindowControlStyleProvider::buttonsFor(QVariantList const& kinds) const
{
    auto named = std::vector<config::WindowControlKind> {};
    for (auto const& kind: kinds)
    {
        auto isInt = false;
        auto const value = kind.toInt(&isInt);
        if (kind.typeId() != QMetaType::QString && isInt && value >= 0
            && std::cmp_less(value, config::WindowControlCount))
            named.push_back(static_cast<config::WindowControlKind>(value));
    }
    return buttonsOf(named);
}

QVariantList WindowControlStyleProvider::buttonsOf(std::span<config::WindowControlKind const> kinds) const
{
    // One list of fully-resolved rows rather than a handful of parallel accessors QML would have to
    // index in step: the QML iterates this with a Repeater and reads each row's own fields, so
    // adding a control -- or a style that orders them differently -- changes no QML at all.
    auto const& tokens = config::windowControlTokens(_style);
    auto const glyphs = config::controlGlyphsFor(tokens, _isFontInstalled);
    auto const font = QString::fromUtf8(glyphs.family);
    auto const wash = washOf(tokens.wash);
    // Dots are inked from the scheme on the Terminal row (config::DotRoles); elsewhere they keep the style's
    // own colours.
    auto const hasDot = _row == config::ChromeStyle::Terminal;
    auto rows = QVariantList {};

    for (auto const kind: kinds)
    {
        auto const glyph = glyphOf(glyphs, kind);

        auto row = QVariantMap {};
        row[QStringLiteral("action")] = actionOf(kind);
        // What the controller keys the caption rectangles by: config::WindowControlKind's value.
        row[QStringLiteral("kind")] = static_cast<int>(kind);
        row[QStringLiteral("glyph")] = glyph;
        // Only maximize has a second state, but every row carries the field so the delegate can read
        // it unconditionally; for the others it simply equals `glyph`.
        row[QStringLiteral("restoreGlyph")] =
            kind == config::WindowControlKind::Maximize ? QString::fromUtf8(glyphs.restore) : glyph;
        // The icon font, or empty for the chrome font.
        row[QStringLiteral("font")] = font;
        row[QStringLiteral("wash")] = wash;
        // A QML-facing bool: whether the dot takes the scheme's status ink below.
        row[QStringLiteral("hasDot")] = hasDot;
        row[QStringLiteral("dotRole")] = static_cast<int>(config::DotRoles[static_cast<size_t>(kind)]);
        row[QStringLiteral("dotColor")] = colorOrUnset(tokens.dotColors[static_cast<size_t>(kind)]);
        // The close button is the only one any style tints on hover, and it is the only one whose
        // hover says something the palette cannot -- so the field is per row rather than per style.
        // Under a contrast theme the palette says everything, the close button's hover included.
        auto const tinted =
            kind == config::WindowControlKind::Close && _contrast == platform::ContrastTheme::None;
        row[QStringLiteral("hoverFill")] = tinted ? colorOrUnset(tokens.closeHoverColor) : QString {};
        // The themed icon a Round control draws, empty on the glyph styles; restoreIconName as restoreGlyph.
        auto const iconName = QString::fromUtf8(tokens.iconNames[static_cast<size_t>(kind)]);
        row[QStringLiteral("iconName")] = iconName;
        auto const restoreIconName = kind == config::WindowControlKind::Maximize
                                         ? QString::fromUtf8(tokens.restoreIconName)
                                         : iconName;
        row[QStringLiteral("restoreIconName")] = restoreIconName;
        // The bundled copies, for a desktop whose icon theme lacks the names.
        row[QStringLiteral("bundledIcon")] = bundledIconOf(iconName);
        row[QStringLiteral("restoreBundledIcon")] = bundledIconOf(restoreIconName);
        rows.push_back(row);
    }

    return rows;
}

} // namespace contour::window
