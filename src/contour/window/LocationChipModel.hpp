// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The location chip as QML reads it: one QObject that exposes the whole chip surface -- where it shows, the
/// daemon's name, the dot, the glyph, the texts at every fit step, the accessible words -- from the domain
/// state it is given. WindowController and the test doubles standing in for it each own one and supply only
/// inputs, so no rule is derived twice.

#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/DaemonHosting.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/LocationChip.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include <string>

namespace contour::window
{

/// What the chip is a function of: the window's domain state, never anything QML measured.
struct LocationChipInputs
{
    std::string daemon; ///< The daemon's name as the chip shows it; empty for an in-process window.
    DaemonHosting hosting = DaemonHosting::InProcess; ///< What serves the window's sessions.
    DaemonLocality locality = DaemonLocality::Local;  ///< Where the daemon runs.
    HostChipState state = HostChipState::Connected;   ///< What the connection looks like.
    std::string workspace;                   ///< The workspace the window shows, as its row titles it.
    Urgency otherWorkspaces = Urgency::Idle; ///< The most urgent roll-up of the other workspaces.
    ChipDisclosure disclosure = ChipDisclosure::Collapsed; ///< Whether the switcher is open.
    BarPresence band = BarPresence::Hidden;                ///< Whether the title band shows.
    BarCentre centre = BarCentre::Empty;                   ///< What the band's middle holds.
    SidebarShown sidebar = SidebarShown::No;               ///< Whether the workspace sidebar shows.

    [[nodiscard]] bool operator==(LocationChipInputs const&) const = default;
};

/// The location chip's surface for QML. Built from its inputs; setInputs() moves it to the next state and
/// tells each group of readers only when something they read changed.
class LocationChipModel: public QObject
{
    Q_OBJECT
    /// Where the chip shows: "none", "band" or "sidebar" (locationChipPlaceName()).
    Q_PROPERTY(QString place READ place NOTIFY placeChanged)
    /// The daemon's name as the chip shows it; empty for an in-process window.
    Q_PROPERTY(QString daemonName READ daemonName NOTIFY surfaceChanged)
    /// Whether the status dot (on the Terminal row, its glyph) shows: the model's call, hidden without a
    /// daemon.
    Q_PROPERTY(bool dotShown READ dotShown NOTIFY surfaceChanged)
    /// A window::HostChipState's underlying value: how the connection looks.
    Q_PROPERTY(int connectionState READ connectionState NOTIFY surfaceChanged)
    /// The terminal-style stand-in for the dot, for the current connection state.
    Q_PROPERTY(QString stateGlyph READ stateGlyph NOTIFY surfaceChanged)
    /// The glyph of every connection state: the band reserves the widest, so the chip's least width does not
    /// move with the connection.
    Q_PROPERTY(QStringList stateGlyphs READ stateGlyphs CONSTANT)
    /// One entry per fit step, in elision order, each { fit, key, daemon, status, workspace, tooltip }: the
    /// window::LocationChipFit's underlying value, the name its width goes by, and what the chip says there.
    Q_PROPERTY(QVariantList steps READ steps NOTIFY surfaceChanged)
    /// What a screen reader says of the chip: what it shows (`Workspace billing on starfive`).
    Q_PROPERTY(QString accessibleName READ accessibleName NOTIFY accessibilityChanged)
    /// What a screen reader says of the control that names the daemon alone (`Daemon local`).
    Q_PROPERTY(QString daemonAccessibleName READ daemonAccessibleName NOTIFY accessibilityChanged)
    /// What a screen reader adds to the chip's name: the connection's state, another workspace's news, and
    /// whether the switcher is open.
    Q_PROPERTY(QString accessibleDescription READ accessibleDescription NOTIFY accessibilityChanged)

  public:
    /// @param inputs The chip's state at construction.
    /// @param parent The QObject parent.
    explicit LocationChipModel(LocationChipInputs inputs, QObject* parent = nullptr);

    /// Moves the chip to @p inputs, notifying place, surface and accessibility readers each only if something
    /// they read changed.
    /// @param inputs The chip's new state.
    void setInputs(LocationChipInputs inputs);

    /// @return The state the chip is in.
    [[nodiscard]] LocationChipInputs const& inputs() const noexcept { return _inputs; }

    /// @param widths What each fit step needs by its key (window::LocationChipSteps), in the same unit as
    ///               `available`, the room the chip has.
    /// @return A window::LocationChipFit's underlying value: the first step that fits.
    [[nodiscard]] Q_INVOKABLE static int fitFor(QVariantMap const& widths);

    [[nodiscard]] QString place() const;
    [[nodiscard]] QString daemonName() const;
    [[nodiscard]] bool dotShown() const noexcept { return _dotShown; }
    [[nodiscard]] int connectionState() const noexcept;
    [[nodiscard]] QString stateGlyph() const;
    [[nodiscard]] static QStringList stateGlyphs();
    [[nodiscard]] QVariantList steps() const { return _steps; }
    [[nodiscard]] QString accessibleName() const;
    [[nodiscard]] QString daemonAccessibleName() const;
    [[nodiscard]] QString accessibleDescription() const;

  signals:
    /// Where the chip shows changed: the band, its centre, what serves the window or the sidebar's
    /// visibility.
    void placeChanged();
    /// What the chip draws changed: the daemon, its dot, the connection, the workspace, the texts.
    void surfaceChanged();
    /// What assistive technology reads changed, which another workspace's urgency or the switcher opening
    /// does without touching the surface.
    void accessibilityChanged();

  private:
    /// @return The chip's texts at every fit step, as the QML list `steps` reads.
    [[nodiscard]] QVariantList buildSteps() const;

    LocationChipInputs _inputs;
    bool _dotShown;      ///< @see dotShown
    QVariantList _steps; ///< @see steps
};

} // namespace contour::window
