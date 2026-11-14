// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationChipModel.hpp>
#include <contour/window/LocationChipQt.hpp>
#include <contour/window/QtString.hpp>

#include <QtCore/QVariantMap>

#include <utility>

namespace contour::window
{

namespace
{
    /// @param inputs The chip's state.
    /// @return Where the chip shows for it.
    [[nodiscard]] LocationChipPlace placeOf(LocationChipInputs const& inputs) noexcept
    {
        return locationChipPlace(inputs.band, inputs.centre, inputs.hosting, inputs.sidebar);
    }

    /// @param inputs The chip's state.
    /// @return Whether the dot shows: only a chip that names a daemon has one.
    [[nodiscard]] bool dotShownFor(LocationChipInputs const& inputs) noexcept
    {
        return !inputs.daemon.empty() && daemonDotOf(inputs.locality, inputs.state) == DaemonDot::Shown;
    }

    /// @param a One state of the chip.
    /// @param b Another.
    /// @return Whether they draw the same: the daemon, its dot, the connection and the workspace.
    [[nodiscard]] bool sameSurface(LocationChipInputs const& a, LocationChipInputs const& b) noexcept
    {
        return a.daemon == b.daemon && a.locality == b.locality && a.state == b.state
               && a.workspace == b.workspace;
    }
} // namespace

LocationChipModel::LocationChipModel(LocationChipInputs inputs, QObject* parent):
    QObject(parent), _inputs { std::move(inputs) }, _dotShown { dotShownFor(_inputs) }, _steps(buildSteps())
{
}

void LocationChipModel::setInputs(LocationChipInputs inputs)
{
    if (inputs == _inputs)
        return;
    auto const surface = !sameSurface(inputs, _inputs);
    auto const place = placeOf(inputs) != placeOf(_inputs);
    auto const accessibility = surface || inputs.otherWorkspaces != _inputs.otherWorkspaces
                               || inputs.disclosure != _inputs.disclosure;
    _inputs = std::move(inputs);
    if (surface)
    {
        _dotShown = dotShownFor(_inputs);
        _steps = buildSteps();
        emit surfaceChanged();
    }
    if (place)
        emit placeChanged();
    if (accessibility)
        emit accessibilityChanged();
}

int LocationChipModel::fitFor(QVariantMap const& widths)
{
    auto const widthOf = [&](std::string_view key) {
        return widths.value(toQString(key)).toDouble();
    };
    return static_cast<int>(
        std::to_underlying(locationChipFit(widthOf, widths.value(QStringLiteral("available")).toDouble())));
}

QString LocationChipModel::place() const
{
    return toQString(locationChipPlaceName(placeOf(_inputs)));
}

QString LocationChipModel::daemonName() const
{
    return toQString(_inputs.daemon);
}

int LocationChipModel::connectionState() const noexcept
{
    return static_cast<int>(std::to_underlying(_inputs.state));
}

QString LocationChipModel::stateGlyph() const
{
    return toQString(daemonStateGlyph(_inputs.state));
}

QStringList LocationChipModel::stateGlyphs()
{
    return locationChipStateGlyphList();
}

QString LocationChipModel::accessibleName() const
{
    return toQString(locationChipAccessibleName(_inputs.daemon, _inputs.workspace, QtTranslated {}));
}

QString LocationChipModel::daemonAccessibleName() const
{
    return toQString(locationChipDaemonAccessibleName(_inputs.daemon, QtTranslated {}));
}

QString LocationChipModel::accessibleDescription() const
{
    return toQString(locationChipAccessibleDescription(
        _inputs.state, _inputs.otherWorkspaces, _inputs.disclosure, QtTranslated {}));
}

QVariantList LocationChipModel::buildSteps() const
{
    auto steps = QVariantList {};
    for (auto const& text:
         locationChipStepTexts(_inputs.state, _inputs.daemon, _inputs.workspace, QtTranslated {}))
        steps.push_back(
            QVariantMap { { QStringLiteral("fit"), static_cast<int>(std::to_underlying(text.fit)) },
                          { QStringLiteral("key"), toQString(text.key) },
                          { QStringLiteral("daemon"), toQString(text.daemon) },
                          { QStringLiteral("status"), toQString(text.status) },
                          { QStringLiteral("workspace"), toQString(text.workspace) },
                          { QStringLiteral("tooltip"), toQString(text.tooltip) } });
    return steps;
}

} // namespace contour::window
