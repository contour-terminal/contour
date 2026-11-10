// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonDriftModel.hpp>

#include <utility>

namespace contour::window
{

DaemonDriftModel::DaemonDriftModel(QObject* parent): QObject(parent)
{
}

bool DaemonDriftModel::isVisible() const noexcept
{
    return _notice.kind != vthost::client::DriftKind::None || _action == DriftAction::Upgrading;
}

QString DaemonDriftModel::text() const
{
    switch (_action)
    {
        case DriftAction::Upgrading: return tr("Upgrading daemon…");
        case DriftAction::Restarting: return tr("Restarting daemon…");
        case DriftAction::Idle: break;
    }
    if (!_failure.isEmpty())
        return _failure;
    return QString::fromStdString(_notice.text);
}

QString DaemonDriftModel::actionLabel() const
{
    return QString::fromUtf8(vthost::client::RestartActionLabel.data(),
                             static_cast<qsizetype>(vthost::client::RestartActionLabel.size()));
}

QString DaemonDriftModel::confirmation() const
{
    return QString::fromStdString(_notice.confirmation);
}

bool DaemonDriftModel::isRestarting() const noexcept
{
    return _action == DriftAction::Restarting;
}

bool DaemonDriftModel::offersUpgrade() const noexcept
{
    return _offer == DriftOffer::UpgradeAndRestart && _upgradeRequest == UpgradeRequest::Settled;
}

QString DaemonDriftModel::upgradeActionLabel() const
{
    return tr("Upgrade daemon (keeps your programs running)");
}

QString DaemonDriftModel::upgradeConfirmation() const
{
    return tr("The daemon replaces itself with the new version. Your programs keep running and the "
              "windows reconnect in a moment.");
}

bool DaemonDriftModel::isUpgrading() const noexcept
{
    return _action == DriftAction::Upgrading;
}

void DaemonDriftModel::show(vthost::client::DriftNotice notice, DriftOffer offer)
{
    // The same drift assessed again after a failed upgrade (the connection came back) is still that
    // failure: its reason and the restart-only offer stay until the release changes.
    auto const sameFailedDrift = !_failedNoticeText.empty() && notice.text == _failedNoticeText;
    _notice = std::move(notice);
    if (_action == DriftAction::Upgrading)
        _action = DriftAction::Idle;
    if (sameFailedDrift)
    {
        _offer = DriftOffer::RestartOnly;
        emit changed();
        return;
    }
    _offer = offer;
    _failedNoticeText.clear();
    _failure.clear();
    emit changed();
}

void DaemonDriftModel::requestRestart()
{
    if (_action != DriftAction::Idle || _upgradeRequest == UpgradeRequest::InFlight)
        return;
    _action = DriftAction::Restarting;
    _failure.clear();
    emit changed();
    emit restartRequested();
}

void DaemonDriftModel::requestUpgrade()
{
    if (!offersUpgrade() || !canBeginUpgrade())
        return;
    beginUpgrade();
}

CommandedUpgradeStart DaemonDriftModel::requestCommandedUpgrade()
{
    if (!canBeginUpgrade())
        return CommandedUpgradeStart::AlreadyUnderWay;
    beginUpgrade();
    return CommandedUpgradeStart::Started;
}

bool DaemonDriftModel::canBeginUpgrade() const noexcept
{
    return _action == DriftAction::Idle && _upgradeRequest == UpgradeRequest::Settled;
}

void DaemonDriftModel::beginUpgrade()
{
    _action = DriftAction::Upgrading;
    _upgradeRequest = UpgradeRequest::InFlight;
    _failure.clear();
    emit changed();
    emit upgradeRequested();
}

void DaemonDriftModel::noteRestartFailed(QString const& reason)
{
    _action = DriftAction::Idle;
    _failure = tr("Restart failed: %1").arg(reason.isEmpty() ? tr("unknown error") : reason);
    emit changed();
}

void DaemonDriftModel::noteUpgradeFailed(QString const& reason)
{
    if (_action == DriftAction::Upgrading)
        _action = DriftAction::Idle;
    _upgradeRequest = UpgradeRequest::Settled;
    _failedNoticeText = _notice.text;
    // The refusal is what the user reads next to the restart that is now the only action left.
    _offer = DriftOffer::RestartOnly;
    _failure = tr("Upgrade failed: %1; restarting the daemon ends its programs")
                   .arg(reason.isEmpty() ? tr("unknown error") : reason);
    emit changed();
    emit upgradeFailed(reason);
}

void DaemonDriftModel::noteUpgradeNotStarted()
{
    if (_action == DriftAction::Upgrading)
        _action = DriftAction::Idle;
    _upgradeRequest = UpgradeRequest::Settled;
    emit changed();
}

void DaemonDriftModel::noteUpgradeEnded()
{
    if (_action != DriftAction::Upgrading)
        return;
    _action = DriftAction::Idle;
    emit changed();
}

void DaemonDriftModel::noteDaemonUpgrading()
{
    if (_action == DriftAction::Upgrading)
        return;
    _action = DriftAction::Upgrading;
    emit changed();
}

void DaemonDriftModel::noteUpgradeRequestDone()
{
    if (_upgradeRequest == UpgradeRequest::Settled)
        return;
    _upgradeRequest = UpgradeRequest::Settled;
    emit changed();
}

DriftReassessment DaemonDriftModel::followConnection(vthost::client::ConnectionState state)
{
    using vthost::client::ConnectionState;
    switch (state)
    {
        case ConnectionState::Upgrading: noteDaemonUpgrading(); return DriftReassessment::Keep;
        case ConnectionState::Connected:
            return _action == DriftAction::Upgrading ? DriftReassessment::Reassess : DriftReassessment::Keep;
        case ConnectionState::Reconnecting:
        case ConnectionState::Refused:
        case ConnectionState::Failed:
        case ConnectionState::Stopped:
            // The daemon did not come back as the upgrade it announced: the line must not stay.
            noteUpgradeEnded();
            return DriftReassessment::Keep;
        case ConnectionState::Connecting: return DriftReassessment::Keep;
    }
    return DriftReassessment::Keep;
}

} // namespace contour::window
