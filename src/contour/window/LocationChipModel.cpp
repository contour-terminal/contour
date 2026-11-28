// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationChipModel.hpp>
#include <contour/window/LocationChipQt.hpp>
#include <contour/window/QtString.hpp>

#include <QtCore/QVariantMap>

#include <string_view>
#include <utility>

namespace contour::window
{

namespace
{
    /// @param inputs The chip's state.
    /// @return What serves the window as far as the chip is concerned: a host with no label to name (a tmux
    ///         attach that has none) has no daemon to show, which is an in-process window's chip.
    [[nodiscard]] DaemonHosting effectiveHostingOf(LocationChipInputs const& inputs) noexcept
    {
        return inputs.daemon.empty() ? DaemonHosting::InProcess : inputs.hosting;
    }

    /// @param inputs The chip's state.
    /// @return Where the chip shows for it.
    [[nodiscard]] LocationChipPlace placeOf(LocationChipInputs const& inputs) noexcept
    {
        return locationChipPlace(
            inputs.band, inputs.centre, effectiveHostingOf(inputs), inputs.sidebar, inputs.state);
    }

    /// @param widths What QML measured: a width per step key, and `available`.
    /// @param segments What the chip names.
    /// @return The fit those widths allow, as its underlying value. A step QML did not measure does not fit
    ///         and a missing `available` is no room, so an unmeasured chip falls to its least step.
    [[nodiscard]] int fitOf(QVariantMap const& widths, ChipSegments segments)
    {
        auto const widthOf = [&](std::string_view key) {
            return measuredStepWidth(widths, key);
        };
        auto const available = widths.value(QStringLiteral("available")).toDouble();
        return static_cast<int>(std::to_underlying(locationChipFit(widthOf, available, segments)));
    }

    /// @param inputs The chip's state.
    /// @return Whether the dot shows: only a chip that names a daemon has one.
    [[nodiscard]] bool dotShownFor(LocationChipInputs const& inputs) noexcept
    {
        // A jump under way shows a connection under way, wherever the window's own daemon runs.
        auto const state = inputs.pendingDaemon.empty() ? inputs.state : HostChipState::Reconnecting;
        return !inputs.daemon.empty() && daemonDotOf(inputs.locality, state) == DaemonDot::Shown;
    }

    /// @param a One state of the chip.
    /// @param b Another.
    /// @return Whether they draw the same: the daemon and what serves it, its dot, the connection and
    ///         the workspace.
    [[nodiscard]] bool sameSurface(LocationChipInputs const& a, LocationChipInputs const& b) noexcept
    {
        return a.daemon == b.daemon && a.hosting == b.hosting && a.locality == b.locality
               && a.state == b.state && a.workspace == b.workspace && a.pendingDaemon == b.pendingDaemon;
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
    return fitOf(widths, ChipSegments::DaemonAndWorkspace);
}

int LocationChipModel::fitForDaemonOnly(QVariantMap const& widths)
{
    return fitOf(widths, ChipSegments::DaemonOnly);
}

QString LocationChipModel::place() const
{
    return toQString(locationChipPlaceName(placeOf(_inputs)));
}

bool LocationChipModel::workspaceShown() const noexcept
{
    return daemonHostingPolicy(effectiveHostingOf(_inputs)).segments == ChipSegments::DaemonAndWorkspace;
}

bool LocationChipModel::switcherControlShown() const noexcept
{
    return daemonHostingPolicy(effectiveHostingOf(_inputs)).sidebar == SidebarNaming::HeadingRow;
}

QString LocationChipModel::daemonName() const
{
    return toQString(_inputs.daemon);
}

int LocationChipModel::connectionState() const noexcept
{
    return static_cast<int>(std::to_underlying(shownState()));
}

QString LocationChipModel::stateGlyph() const
{
    return toQString(daemonStateGlyph(shownState()));
}

HostChipState LocationChipModel::shownState() const noexcept
{
    // A jump connects elsewhere: the dot and the words take a connection's look under way, not the window's
    // own link's.
    return jumpPending() ? HostChipState::Reconnecting : _inputs.state;
}

QStringList LocationChipModel::stateGlyphs()
{
    return locationChipStateGlyphList();
}

QString LocationChipModel::accessibleName() const
{
    if (jumpPending())
        return toQString(locationChipPendingAccessibleName(_inputs.pendingDaemon, QtTranslated {}));
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
    auto const texts =
        jumpPending()
            ? locationChipPendingStepTexts(_inputs.pendingDaemon, QtTranslated {})
            : locationChipStepTexts(_inputs.state, _inputs.daemon, _inputs.workspace, QtTranslated {});
    for (auto const& text: texts)
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
