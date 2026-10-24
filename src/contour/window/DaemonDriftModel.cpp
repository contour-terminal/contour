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
    return _notice.kind != vthost::client::DriftKind::None;
}

QString DaemonDriftModel::text() const
{
    if (_isRestarting)
        return tr("Restarting daemon…");
    if (!_failure.isEmpty())
        return tr("Restart failed: %1").arg(_failure);
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
    return _isRestarting;
}

void DaemonDriftModel::show(vthost::client::DriftNotice notice)
{
    _notice = std::move(notice);
    _failure.clear();
    emit changed();
}

void DaemonDriftModel::requestRestart()
{
    if (_isRestarting)
        return;
    _isRestarting = true;
    _failure.clear();
    emit changed();
    emit restartRequested();
}

void DaemonDriftModel::noteRestartFailed(QString reason)
{
    _isRestarting = false;
    _failure = reason.isEmpty() ? tr("unknown error") : std::move(reason);
    emit changed();
}

} // namespace contour::window
