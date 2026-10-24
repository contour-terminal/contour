// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <vthost/client/ReleaseDrift.hpp>

namespace contour::window
{

/// What the daemon-drift badge shows: whether the daemon runs another release than this client, the
/// line saying so, and the state of a restart the user asked for.
///
/// The decisions (what drifts, what a restart costs) are made by vthost::client::assessDrift(); this
/// model only holds the outcome for QML and tracks the restart's progress.
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

    /// Replaces the shown drift; the badge is visible iff the notice names one.
    /// @param notice What assessDrift() found.
    void show(vthost::client::DriftNotice notice);

    /// Starts a restart; ignored while one is under way. Emits restartRequested().
    Q_INVOKABLE void requestRestart();

    /// Ends the restart that failed and shows why.
    /// @param reason The failure, in words for the user.
    void noteRestartFailed(QString reason);

  signals:
    /// Anything the properties expose changed.
    void changed();
    /// The user confirmed a restart.
    void restartRequested();

  private:
    vthost::client::DriftNotice _notice;
    QString _failure;
    bool _isRestarting = false;
};

} // namespace contour::window
