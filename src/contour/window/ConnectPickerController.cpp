// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/window/ConnectPickerController.hpp>

#include <core/log/LogStore.hpp>

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <utility>

namespace contour::window
{

namespace
{
    /// What a group's first row announces.
    [[nodiscard]] QString groupTitleOf(cli::ConnectGroup group)
    {
        switch (group)
        {
            case cli::ConnectGroup::Typed: return {};
            case cli::ConnectGroup::ThisComputer: return QObject::tr("This computer");
            case cli::ConnectGroup::SavedHosts: return QObject::tr("Saved hosts");
        }
        return {};
    }

    constexpr auto HostsSectionPage = std::string_view { "section:hosts" };
} // namespace

ConnectPickerController::ConnectPickerController(ConnectPickerServices services, QObject* parent):
    QAbstractListModel(parent), _services(std::move(services))
{
    refresh();
}

ConnectPickerController::~ConnectPickerController() = default;

void ConnectPickerController::refresh()
{
    // What was asked has reached the configuration by now, or has yet to: keep showing it until it has.
    if (_askOverride == _services.config().connectPickerOnStart.value())
        _askOverride.reset();
    _filter.clear();
    auto const hadFailure = !_failure.isEmpty();
    _failure.clear();
    _failedTarget.clear();
    _daemon = cli::LocalDaemonState { .run = cli::DaemonRunState::Probing, .sessions = std::nullopt };
    rebuild(_services.state.load().value_or(config::GuiState {}).lastConnectTarget);
    probeDaemon();
    emit filterChanged();
    if (hadFailure)
        emit failureChanged();
}

void ConnectPickerController::probeDaemon()
{
    // Off the GUI thread, like a host's check: a daemon that accepts and never answers holds the probe for
    // its whole timeout. The worker never touches this object; its answer is posted back.
    auto const probe = ++_probe;
    _workers.emplace_back(
        [this, probe, job = _services.daemon(), guard = QPointer<ConnectPickerController> { this }] {
            auto state = job();
            QMetaObject::invokeMethod(
                guard.data(),
                [this, probe, state] {
                    if (probe != _probe)
                        return;
                    _daemon = state;
                    // The selection stays on what the user has reached while the probe was asking.
                    rebuild(_current >= 0 ? std::optional { _rows[static_cast<std::size_t>(_current)].text }
                                          : std::nullopt);
                },
                Qt::QueuedConnection);
        });
}

void ConnectPickerController::rebuild(std::optional<std::string> keep)
{
    beginResetModel();
    _rows = cli::connectRows({ .hosts = _services.config().hosts.value(), .daemon = _daemon },
                             _filter.toStdString());
    endResetModel();
    auto const index = keep ? static_cast<int>(cli::initialSelection(_rows, *keep)) : 0;
    _current = _rows.empty() ? -1 : index;
    emit currentIndexChanged();
}

void ConnectPickerController::setFilter(QString const& filter)
{
    if (_filter == filter)
        return;
    _filter = filter;
    rebuild(std::nullopt);
    emit filterChanged();
}

void ConnectPickerController::setCurrentIndex(int index)
{
    auto const clamped = _rows.empty() ? -1 : std::clamp(index, 0, static_cast<int>(_rows.size()) - 1);
    if (_current == clamped)
        return;
    _current = clamped;
    emit currentIndexChanged();
}

void ConnectPickerController::step(int delta)
{
    setCurrentIndex(_current + delta);
}

void ConnectPickerController::activate(int row)
{
    if (_isBusy || row < 0)
        return;
    auto const index = static_cast<std::size_t>(row);
    if (index >= _rows.size())
        return;
    connectTo(_rows[index].target);
}

void ConnectPickerController::activateCurrent()
{
    activate(_current);
}

void ConnectPickerController::quickSelect(int digit)
{
    if (digit < 1)
        return;
    if (auto const row = cli::rowForQuickSelect(_rows, static_cast<std::size_t>(digit)))
        activate(static_cast<int>(*row));
}

void ConnectPickerController::retry()
{
    if (!_isBusy && _lastAttempt)
        connectTo(*_lastAttempt);
}

void ConnectPickerController::connectTo(config::LaunchTarget const& target)
{
    _lastAttempt = target;
    if (!_failure.isEmpty())
    {
        _failure.clear();
        _failedTarget.clear();
        emit failureChanged();
        notifyFailureRows();
    }
    setBusy(true);
    // Checked off the GUI thread: an ssh host takes seconds to answer or to fail. The worker never touches
    // this object; its answer is posted back, and a controller gone by then simply ignores it.
    // A cancelled check keeps running to its end, so each gets its own thread, joined with the controller.
    auto const attempt = ++_attempt;
    _workers.emplace_back([this,
                           attempt,
                           target,
                           check = _services.check(target),
                           guard = QPointer<ConnectPickerController> { this }] {
        auto result = check();
        QMetaObject::invokeMethod(
            guard.data(),
            [this, attempt, target, result = std::move(result)]() mutable {
                finishCheck(attempt, target, std::move(result));
            },
            Qt::QueuedConnection);
    });
}

void ConnectPickerController::finishCheck(std::uint64_t attempt,
                                          config::LaunchTarget const& target,
                                          std::expected<void, std::string> result)
{
    // Cancelled, or superseded: nothing is waiting for this answer, and it must not open a window.
    if (attempt != _attempt)
        return;
    setBusy(false);
    if (!result)
    {
        showFailure(target, result.error());
        return;
    }
    launch(target);
}

void ConnectPickerController::launch(config::LaunchTarget const& target)
{
    if (auto const started = _services.launch(target); !started)
    {
        showFailure(target, started.error());
        return;
    }
    // A saved address is a typed one's twin: both are remembered by their text, and a host that is later
    // removed simply falls back to the first row.
    auto const text = config::toString(target);
    std::ignore = _services.state.update([&](config::GuiState& state) { state.lastConnectTarget = text; });
    emit connected();
}

void ConnectPickerController::editFailedHost() const
{
    if (failureCanEditHost())
        _services.showSettingsPage("host:" + _failedTarget.toStdString());
}

void ConnectPickerController::manageHosts() const
{
    _services.showSettingsPage(std::string { HostsSectionPage });
}

void ConnectPickerController::abandon()
{
    ++_attempt; // whatever check is in flight is no longer wanted
    setBusy(false);
}

void ConnectPickerController::cancel()
{
    abandon();
    emit cancelled();
}

bool ConnectPickerController::failureCanEditHost() const
{
    return !_failedTarget.isEmpty() && _services.config().hosts.value().contains(_failedTarget.toStdString());
}

bool ConnectPickerController::askEveryTime() const
{
    return _askOverride.value_or(_services.config().connectPickerOnStart.value());
}

void ConnectPickerController::setAskEveryTime(bool ask)
{
    if (askEveryTime() == ask)
        return;
    _services.setStartPrompt(ask ? StartPrompt::Ask : StartPrompt::Never);
    // The stored value reaches the configuration with the next reload; the switch shows what was asked.
    _askOverride = ask;
    emit askEveryTimeChanged();
}

QString ConnectPickerController::quickModifier() const
{
    return _services.quickKey == cli::QuickSelectKey::Command ? QStringLiteral("Meta")
                                                              : QStringLiteral("Alt");
}

QString ConnectPickerController::focusModifier() const
{
    return _services.quickKey == cli::QuickSelectKey::Command ? QStringLiteral("Meta")
                                                              : QStringLiteral("Ctrl");
}

void ConnectPickerController::showFailure(config::LaunchTarget const& target, std::string const& reason)
{
    _failure = QString::fromStdString(reason);
    _failedTarget = QString::fromStdString(config::toString(target));
    emit failureChanged();
    notifyFailureRows();
}

void ConnectPickerController::notifyFailureRows()
{
    if (!_rows.empty())
        emit dataChanged(index(0), index(rowCount() - 1), { static_cast<int>(Roles::FailureRole) });
}

void ConnectPickerController::setBusy(bool busy)
{
    if (_isBusy == busy)
        return;
    _isBusy = busy;
    emit busyChanged();
}

QVariant ConnectPickerController::data(QModelIndex const& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(_rows.size()))
        return {};
    auto const row = static_cast<std::size_t>(index.row());
    auto const& entry = _rows[row];
    switch (static_cast<Roles>(role))
    {
        case Roles::TitleRole: return QString::fromStdString(entry.title);
        case Roles::DetailRole: return QString::fromStdString(entry.detail);
        case Roles::TagRole: return QString::fromStdString(entry.tag);
        case Roles::GroupTitleRole:
            return row == 0 || _rows[row - 1].group != entry.group ? groupTitleOf(entry.group) : QString {};
        case Roles::QuickRole:
            return entry.quick
                       ? QString::fromStdString(cli::quickSelectLabel(_services.quickKey, *entry.quick))
                       : QString {};
        case Roles::TargetRole: return QString::fromStdString(entry.text);
        case Roles::FailureRole:
            return !_failure.isEmpty() && QString::fromStdString(entry.text) == _failedTarget ? _failure
                                                                                              : QString {};
    }
    return {};
}

int ConnectPickerController::rowCount(QModelIndex const& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_rows.size());
}

QHash<int, QByteArray> ConnectPickerController::roleNames() const
{
    return {
        { static_cast<int>(Roles::TitleRole), "title" },
        { static_cast<int>(Roles::DetailRole), "detail" },
        { static_cast<int>(Roles::TagRole), "tag" },
        { static_cast<int>(Roles::GroupTitleRole), "groupTitle" },
        { static_cast<int>(Roles::QuickRole), "quick" },
        { static_cast<int>(Roles::TargetRole), "target" },
        { static_cast<int>(Roles::FailureRole), "rowFailure" },
    };
}

std::unique_ptr<QQuickWindow> loadConnectPickerWindow(QQmlEngine& engine, ConnectPickerController& picker)
{
    auto component = QQmlComponent(&engine);
    component.loadFromModule("Contour.Ui", "ConnectPickerWindow");
    if (!component.isReady())
    {
        errorLog()("Could not load the Connect picker: {}", component.errorString().toStdString());
        return nullptr;
    }
    auto object = std::unique_ptr<QObject>(component.createWithInitialProperties(
        QVariantMap { { QStringLiteral("picker"), QVariant::fromValue(&picker) } }));
    if (qobject_cast<QQuickWindow*>(object.get()) == nullptr)
    {
        errorLog()("The Connect picker did not build a window: {}",
                   object ? object->metaObject()->className() : component.errorString().toStdString());
        return nullptr;
    }
    return std::unique_ptr<QQuickWindow>(static_cast<QQuickWindow*>(object.release()));
}

} // namespace contour::window
