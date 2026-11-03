// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/DaemonDriftChoice.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>
#include <string>

#include <vthost/client/ConnectionState.hpp>
#include <vthost/client/ReleaseDrift.hpp>

namespace contour::window
{

/// What the drift chip shows under way.
enum class DriftAction : uint8_t
{
    Idle = 0,   ///< Nothing: the drift, or the failure of what was asked.
    Restarting, ///< The restart the user asked for.
    Upgrading,  ///< The daemon upgrades, whoever asked.
};

/// Whether this client's own upgrade request is still being answered. Apart from DriftAction::Upgrading,
/// which a reconnect ends while the request may still be settling how the upgrade ended.
enum class UpgradeRequest : uint8_t
{
    Settled = 0, ///< None runs: another may be asked for.
    InFlight,    ///< One runs, until its worker reports back.
};

/// What the caller does with the drift once the connection changed.
enum class DriftReassessment : uint8_t
{
    Keep = 0, ///< Nothing.
    Reassess, ///< Assess the drift anew and show() it: the daemon came back from an upgrade.
};

/// What the daemon-drift badge shows: whether the daemon runs another release than this client, the
/// line saying so, and the state of a restart the user asked for.
///
/// The decisions (what drifts, what a restart costs, whether a live upgrade is offered) are made by
/// vthost::client::assessDrift() and driftOfferFor(); this model only holds the outcome for QML and tracks
/// the progress of the restart or upgrade the user asked for.
class DaemonDriftModel: public QObject
{
    Q_OBJECT

    /// Whether the badge is shown. A plain `bool` because QML reads it as a predicate: a documented
    /// exception to the enum-class rule (QML-facing Q_PROPERTY).
    Q_PROPERTY(bool isVisible READ isVisible NOTIFY changed)
    /// The badge line: the drift, "Restarting daemon…" while restarting, or the failure of a restart.
    Q_PROPERTY(QString text READ text NOTIFY changed)
    /// The label of the restart action, also the confirmation dialog's heading.
    Q_PROPERTY(QString actionLabel READ actionLabel CONSTANT)
    /// What a restart costs, for the confirmation dialog.
    Q_PROPERTY(QString confirmation READ confirmation NOTIFY changed)
    /// Whether a restart is under way. A plain `bool` for the same reason as isVisible.
    Q_PROPERTY(bool isRestarting READ isRestarting NOTIFY changed)
    /// Whether the chip's action is a live upgrade, with a restart only as its fallback. A plain `bool` for
    /// the same reason as isVisible.
    Q_PROPERTY(bool offersUpgrade READ offersUpgrade NOTIFY changed)
    /// The label of the upgrade action, also the upgrade dialog's heading.
    Q_PROPERTY(QString upgradeActionLabel READ upgradeActionLabel CONSTANT)
    /// What an upgrade costs (nothing a program notices), for the upgrade dialog.
    Q_PROPERTY(QString upgradeConfirmation READ upgradeConfirmation CONSTANT)
    /// Whether the daemon is upgrading, whoever asked. A plain `bool` for the same reason as isVisible.
    Q_PROPERTY(bool isUpgrading READ isUpgrading NOTIFY changed)

  public:
    explicit DaemonDriftModel(QObject* parent = nullptr);

    /// @return True while there is a drift to show.
    [[nodiscard]] bool isVisible() const noexcept;
    /// @return The badge line.
    [[nodiscard]] QString text() const;
    /// @return The label of the restart action.
    [[nodiscard]] QString actionLabel() const;
    /// @return What a restart costs.
    [[nodiscard]] QString confirmation() const;
    /// @return True while a restart is under way.
    [[nodiscard]] bool isRestarting() const noexcept;
    /// @return True while the chip offers a live upgrade: one is on offer, and no request of this client's
    ///         is still being answered.
    [[nodiscard]] bool offersUpgrade() const noexcept;
    /// @return The label of the upgrade action.
    [[nodiscard]] QString upgradeActionLabel() const;
    /// @return What an upgrade costs.
    [[nodiscard]] QString upgradeConfirmation() const;
    /// @return True while the daemon is upgrading.
    [[nodiscard]] bool isUpgrading() const noexcept;

    /// Replaces the shown drift; the badge is visible iff the notice names one. Ends an upgrade shown as
    /// under way: the daemon came back, and this is what it runs now.
    /// @param notice What assessDrift() found.
    /// @param offer What the user may do about it.
    void show(vthost::client::DriftNotice notice, DriftOffer offer = DriftOffer::RestartOnly);

    /// Starts a restart; ignored while one is under way, the daemon upgrades, or an upgrade request is
    /// still being answered. Emits restartRequested().
    Q_INVOKABLE void requestRestart();

    /// Starts an upgrade; ignored unless an upgrade is offered and nothing else is under way. Emits
    /// upgradeRequested(); the request is in flight until noteUpgradeFailed() or noteUpgradeRequestDone().
    Q_INVOKABLE void requestUpgrade();

    /// Ends the restart that failed and shows why.
    /// @param reason The failure, in words for the user.
    void noteRestartFailed(QString reason);

    /// Ends the upgrade that failed and shows why. The restart stays on offer, as the fallback.
    /// @param reason The refusal or failure, in words for the user.
    void noteUpgradeFailed(QString const& reason);

    /// Ends an upgrade shown as under way that did not end in the daemon's return (the connection went on
    /// to reconnect, or failed). Shows nothing of its own: the drift line is back.
    void noteUpgradeEnded();

    /// Shows that the daemon is upgrading, though this client did not ask (another client did). The next
    /// show() ends it.
    void noteDaemonUpgrading();

    /// Ends this client's upgrade request that succeeded: another may be asked for. Shows nothing, as the
    /// connection's return shows the daemon's new release.
    void noteUpgradeRequestDone();

    /// Follows the connection to the daemon through an upgrade: "Upgrading daemon…" while it is held,
    /// whichever client asked, and gone again once the daemon is back or the connection is lost. For every
    /// connection, local or remote: a daemon another client upgrades holds every window's connection.
    /// @param state Where the connection stands now.
    /// @return Reassess when the daemon came back from an upgrade; the drift is then assessed anew.
    [[nodiscard]] DriftReassessment followConnection(vthost::client::ConnectionState state);

  signals:
    /// Anything the properties expose changed.
    void changed();
    /// The user confirmed a restart.
    void restartRequested();
    /// The user confirmed a live upgrade.
    void upgradeRequested();

  private:
    vthost::client::DriftNotice _notice;
    DriftOffer _offer = DriftOffer::None;
    QString _failure;
    /// The drift notice an upgrade failed for: assessing that same drift again keeps the failure.
    std::string _failedNoticeText;
    DriftAction _action = DriftAction::Idle;
    UpgradeRequest _upgradeRequest = UpgradeRequest::Settled;
};

} // namespace contour::window
