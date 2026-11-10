// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SwitcherController.hpp>

#include <core/log/LogStore.hpp>

#include <QtCore/QPointer>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

#include <vthost/SocketPath.hpp>

namespace contour::window
{

namespace
{
    using vthost::proto::SwitchDaemonKind;

    /// @return What a group's first row announces; a pick's header is the controller's.
    [[nodiscard]] QString groupTitleOf(SwitcherGroup group)
    {
        switch (group)
        {
            case SwitcherGroup::ThisComputer: return QObject::tr("This computer");
            case SwitcherGroup::SavedHosts: return QObject::tr("Saved hosts");
            case SwitcherGroup::Pick: return {};
        }
        return {};
    }

    /// @return The name QML tells a row's kind by.
    [[nodiscard]] QString kindName(SwitcherRowKind kind)
    {
        switch (kind)
        {
            case SwitcherRowKind::Daemon: return QStringLiteral("daemon");
            case SwitcherRowKind::Workspace: return QStringLiteral("workspace");
            case SwitcherRowKind::NewWorkspace: return QStringLiteral("new-workspace");
            case SwitcherRowKind::NewDaemon: return QStringLiteral("new-daemon");
        }
        return {};
    }

    /// @return A local daemon's address.
    [[nodiscard]] session::SwitchTarget labelled(std::string label)
    {
        return session::SwitchTarget { .kind = SwitchDaemonKind::Label,
                                       .daemon = std::move(label),
                                       .workspace = {} };
    }

    /// @return Whether @p daemon is the one @p address names.
    [[nodiscard]] bool isAt(SwitcherDaemon const& daemon, session::SwitchTarget const& address)
    {
        return daemon.address.kind == address.kind && daemon.address.daemon == address.daemon;
    }
} // namespace

SwitcherController::SwitcherController(SwitcherServices services, QObject* parent):
    QAbstractListModel(parent), _services(std::move(services))
{
}

SwitcherController::~SwitcherController() = default;

void SwitcherController::open(SwitcherOpening opening)
{
    ++_openings;
    _opening = std::move(opening);
    _filter.clear();
    _selectionOrigin = SwitcherSelectionOrigin::Opening;
    _localDaemons.clear();
    _daemons.clear();
    setBusy(false);
    setLabelState({}, {});
    if (_opening.pick)
        _daemons.push_back(*_opening.pick);
    else
    {
        // The saved hosts, and the window's own daemon: first when no label names it (`--connect-tcp`), else
        // in its group -- a host it was attached to by address alone after the saved ones.
        auto const& own = _opening.windowDaemon;
        _daemons = _services.savedHosts();
        if (std::ranges::none_of(_daemons, [&](auto const& each) { return isAt(each, own); }))
        {
            auto ownDaemon =
                SwitcherDaemon { .address = { .kind = own.kind, .daemon = own.daemon, .workspace = {} },
                                 .workspaceCount = std::nullopt,
                                 .tag = {},
                                 .expansion = SwitcherExpansion::Collapsed,
                                 .listing = SwitcherListing::NotListed,
                                 .failure = {},
                                 .workspaces = {} };
            if (own.kind == SwitchDaemonKind::Host)
                _daemons.push_back(std::move(ownDaemon));
            else
                _daemons.insert(_daemons.begin(), std::move(ownDaemon));
        }
        // The window's own is expanded, and listed now; every other waits for its expansion.
        if (auto const found =
                std::ranges::find_if(_daemons, [&](auto const& each) { return isAt(each, own); });
            found != _daemons.end())
        {
            found->expansion = SwitcherExpansion::Expanded;
            listDaemon(static_cast<std::size_t>(std::distance(_daemons.begin(), found)));
        }
        discoverDaemons();
    }
    setPrompt(_opening.prompt);
    rebuild(std::nullopt);
    emit openingChanged();
    emit filterChanged();
}

void SwitcherController::discoverDaemons()
{
    // Off the GUI thread: a socket that accepts and never answers holds the probe for its whole timeout. The
    // worker never touches this object; its answer is posted back, and the controller outlives its workers.
    auto const opening = _openings;
    _jobs.run([finder = _services.discovery] { return finder->discover(); },
              [this, opening](std::vector<cli::DiscoveredDaemon> const& found) {
                  if (opening != _openings)
                      return;
                  _localDaemons = found;
                  mergeLocalDaemons(found);
              });
}

void SwitcherController::mergeLocalDaemons(std::vector<cli::DiscoveredDaemon> const& found)
{
    auto const keep = selectedTarget();
    auto merged = std::vector<SwitcherDaemon> {};
    // An unnameable own daemon stays first; then every label discovery found, in its order (with the
    // window's own label kept where discovery has not seen it yet); the saved hosts last.
    for (auto& daemon: _daemons)
        if (daemon.address.kind == SwitchDaemonKind::Same)
            merged.push_back(std::move(daemon));
    auto const known = [this](session::SwitchTarget const& address) {
        return std::ranges::find_if(_daemons, [&](auto const& each) { return isAt(each, address); });
    };
    for (auto const& each: found)
    {
        auto const address = labelled(each.label);
        if (auto const existing = known(address); existing != _daemons.end())
        {
            existing->workspaceCount = each.workspaces;
            merged.push_back(std::move(*existing));
            existing->address = {};
        }
        else
            merged.push_back(SwitcherDaemon { .address = address,
                                              .workspaceCount = each.workspaces,
                                              .tag = {},
                                              .expansion = SwitcherExpansion::Collapsed,
                                              .listing = SwitcherListing::NotListed,
                                              .failure = {},
                                              .workspaces = {} });
    }
    for (auto& daemon: _daemons)
        if (daemon.address.kind == SwitchDaemonKind::Label && !daemon.address.daemon.empty())
            merged.push_back(std::move(daemon));
    for (auto& daemon: _daemons)
        if (daemon.address.kind == SwitchDaemonKind::Host)
            merged.push_back(std::move(daemon));
    _daemons = std::move(merged);
    rebuild(keep);
}

void SwitcherController::listDaemon(std::size_t index)
{
    auto& daemon = _daemons[index];
    if (daemon.listing == SwitcherListing::Listing)
        return;
    daemon.listing = SwitcherListing::Listing;
    daemon.failure.clear();
    // Answered later on the GUI thread, found again by its address: the list may have changed meanwhile, and
    // the controller may be gone.
    _services.list(_opening.window,
                   daemon.address,
                   [self = QPointer { this }, opening = _openings, address = daemon.address](auto listed) {
                       if (!self || opening != self->_openings)
                           return;
                       auto const found = std::ranges::find_if(
                           self->_daemons, [&](auto const& each) { return isAt(each, address); });
                       if (found == self->_daemons.end())
                           return;
                       if (listed)
                       {
                           found->listing = SwitcherListing::Listed;
                           found->workspaces = *std::move(listed);
                       }
                       else
                       {
                           found->listing = SwitcherListing::Failed;
                           found->failure = listed.error();
                       }
                       self->rebuild(self->selectedTarget());
                   });
}

void SwitcherController::rebuild(std::optional<session::SwitchTarget> const& keep)
{
    beginResetModel();
    _rows =
        _prompt == SwitcherPrompt::NewDaemon
            ? std::vector<SwitcherRow> {}
            : switcherRows({ .daemons = _daemons,
                             .windowDaemon = _opening.windowDaemon,
                             .scope = _opening.pick ? SwitcherScope::OneDaemon : SwitcherScope::Everything },
                           _filter.toStdString());
    endResetModel();
    // Until the user takes the selection, it is the popup's: it goes to the first daemon that is not this
    // window's, again each time discovery fills the list in.
    auto const untouched = _selectionOrigin == SwitcherSelectionOrigin::Opening && _filter.isEmpty();
    if (_rows.empty())
        _current = -1;
    else if (untouched)
        _current = static_cast<int>(switcherInitialSelection(_rows, _opening.windowDaemon));
    else
        _current = static_cast<int>(switcherSelection(_rows, keep));
    emit currentIndexChanged();
}

std::optional<session::SwitchTarget> SwitcherController::selectedTarget() const
{
    if (_current < 0 || static_cast<std::size_t>(_current) >= _rows.size())
        return std::nullopt;
    return _rows[static_cast<std::size_t>(_current)].target;
}

void SwitcherController::setFilter(QString const& filter)
{
    if (_filter == filter)
        return;
    _filter = filter;
    _selectionOrigin = SwitcherSelectionOrigin::User;
    rebuild(std::nullopt);
    emit filterChanged();
}

void SwitcherController::setCurrentIndex(int index)
{
    auto const clamped = _rows.empty() ? -1 : std::clamp(index, 0, static_cast<int>(_rows.size()) - 1);
    _selectionOrigin = SwitcherSelectionOrigin::User;
    if (_current == clamped)
        return;
    _current = clamped;
    emit currentIndexChanged();
}

void SwitcherController::step(int delta)
{
    setCurrentIndex(_current + delta);
}

void SwitcherController::activateCurrent()
{
    activate(_current);
}

void SwitcherController::openCurrent()
{
    if (_current >= 0 && static_cast<std::size_t>(_current) < _rows.size())
        choose(static_cast<std::size_t>(_current), session::SwitchMode::Open);
}

void SwitcherController::activate(int row)
{
    if (row >= 0 && static_cast<std::size_t>(row) < _rows.size())
        choose(static_cast<std::size_t>(row), _opening.mode);
}

void SwitcherController::choose(std::size_t row, session::SwitchMode mode)
{
    auto const& chosen = _rows[row];
    switch (chosen.kind)
    {
        case SwitcherRowKind::NewDaemon:
            setPrompt(SwitcherPrompt::NewDaemon);
            rebuild(std::nullopt);
            return;
        case SwitcherRowKind::Daemon:
        case SwitcherRowKind::Workspace:
        case SwitcherRowKind::NewWorkspace: finishWith(chosen.target, mode); return;
    }
}

void SwitcherController::finishWith(session::SwitchTarget target, session::SwitchMode mode)
{
    // The opening ends here: whatever answers later for it is no longer wanted.
    ++_openings;
    _services.request(_opening.window, std::move(target), mode);
    emit finished();
}

void SwitcherController::expandCurrent()
{
    if (_current < 0 || static_cast<std::size_t>(_current) >= _rows.size())
        return;
    auto const& row = _rows[static_cast<std::size_t>(_current)];
    if (row.kind != SwitcherRowKind::Daemon || row.expansion == SwitcherExpansion::Expanded)
        return;
    toggle(_current);
}

void SwitcherController::collapseCurrent()
{
    if (_current < 0 || static_cast<std::size_t>(_current) >= _rows.size() || _opening.pick)
        return;
    auto const& row = _rows[static_cast<std::size_t>(_current)];
    if (row.kind == SwitcherRowKind::NewDaemon)
        return;
    auto& daemon = _daemons[row.daemon];
    if (daemon.expansion == SwitcherExpansion::Collapsed)
        return;
    daemon.expansion = SwitcherExpansion::Collapsed;
    // The selection moves to the daemon whose workspaces went.
    rebuild(daemon.address);
}

void SwitcherController::toggle(int row)
{
    if (row < 0 || static_cast<std::size_t>(row) >= _rows.size() || _opening.pick)
        return;
    auto const& chosen = _rows[static_cast<std::size_t>(row)];
    if (chosen.kind != SwitcherRowKind::Daemon)
        return;
    auto const index = chosen.daemon;
    auto& daemon = _daemons[index];
    auto const target = chosen.target;
    if (daemon.expansion == SwitcherExpansion::Expanded)
        daemon.expansion = SwitcherExpansion::Collapsed;
    else
    {
        daemon.expansion = SwitcherExpansion::Expanded;
        // A saved host not yet connected connects now; a failed listing is asked again.
        if (daemon.listing != SwitcherListing::Listed)
            listDaemon(index);
    }
    rebuild(target);
}

void SwitcherController::checkLabel(QString const& label)
{
    auto const text = label.toStdString();
    if (text.empty())
    {
        setLabelState({}, {});
        return;
    }
    if (auto const valid = vthost::validateSocketLabel(text); !valid)
    {
        setLabelState(QString::fromStdString(vthost::describe(valid.error())), {});
        return;
    }
    auto const running =
        std::ranges::contains(_localDaemons, text, &cli::DiscoveredDaemon::label) ? label : QString {};
    setLabelState({}, running);
}

void SwitcherController::submitLabel(QString const& label)
{
    if (_isBusy)
        return;
    auto const text = label.toStdString();
    if (auto const valid = vthost::validateSocketLabel(text); !valid)
    {
        setLabelState(QString::fromStdString(vthost::describe(valid.error())), {});
        return;
    }
    // Known to run: offered, not started (spec: a label already running is refused with an offer to switch).
    if (std::ranges::contains(_localDaemons, text, &cli::DiscoveredDaemon::label))
    {
        setLabelState({}, label);
        return;
    }
    setLabelState({}, {});
    setBusy(true);
    // Started off the GUI thread: the spawn waits for the daemon to serve. The worker never touches this
    // object; its answer is posted back (as discovery's is).
    auto const opening = _openings;
    _jobs.run([job = _services.startDaemon(text)] { return job(); },
              [this, opening, text](std::expected<cli::NamedDaemonStart, std::string> result) {
                  finishStart(opening, text, std::move(result));
              });
}

void SwitcherController::finishStart(std::uint64_t opening,
                                     std::string const& label,
                                     std::expected<cli::NamedDaemonStart, std::string> result)
{
    // Cancelled, or the popup opened again: nothing is waiting for this answer, and it must not open a
    // window.
    if (opening != _openings)
        return;
    setBusy(false);
    if (!result)
    {
        setLabelState(
            QString::fromStdString(std::format("Could not start daemon \"{}\": {}.", label, result.error())),
            {});
        return;
    }
    switch (*result)
    {
        case cli::NamedDaemonStart::AlreadyRunning: setLabelState({}, QString::fromStdString(label)); return;
        case cli::NamedDaemonStart::Started: finishWith(labelled(label), session::SwitchMode::Open); return;
    }
}

void SwitcherController::switchToRunning()
{
    if (_runningLabel.isEmpty() || _isBusy)
        return;
    finishWith(labelled(_runningLabel.toStdString()), session::SwitchMode::Switch);
}

void SwitcherController::abandon()
{
    ++_openings; // whatever is in flight is no longer wanted
    setBusy(false);
}

void SwitcherController::cancel()
{
    abandon();
    emit cancelled();
}

QString SwitcherController::pickTitle() const
{
    if (!_opening.pick)
        return {};
    return tr("Workspaces on %1").arg(QString::fromStdString(switcherDaemonName(_opening.pick->address)));
}

void SwitcherController::setPrompt(SwitcherPrompt prompt)
{
    if (_prompt == prompt)
        return;
    _prompt = prompt;
    emit promptChanged();
}

void SwitcherController::setLabelState(QString error, QString running)
{
    if (_labelError == error && _runningLabel == running)
        return;
    _labelError = std::move(error);
    _runningLabel = std::move(running);
    emit labelChanged();
}

void SwitcherController::setBusy(bool busy)
{
    if (_isBusy == busy)
        return;
    _isBusy = busy;
    emit busyChanged();
}

QVariant SwitcherController::data(QModelIndex const& index, int role) const
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
            if (row != 0 && _rows[row - 1].group == entry.group)
                return QString {};
            return entry.group == SwitcherGroup::Pick ? pickTitle() : groupTitleOf(entry.group);
        case Roles::KindRole: return kindName(entry.kind);
        case Roles::ExpandedRole: return entry.expansion == SwitcherExpansion::Expanded;
    }
    return {};
}

int SwitcherController::rowCount(QModelIndex const& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_rows.size());
}

QHash<int, QByteArray> SwitcherController::roleNames() const
{
    return {
        { static_cast<int>(Roles::TitleRole), "title" },
        { static_cast<int>(Roles::DetailRole), "detail" },
        { static_cast<int>(Roles::TagRole), "tag" },
        { static_cast<int>(Roles::GroupTitleRole), "groupTitle" },
        { static_cast<int>(Roles::KindRole), "kind" },
        { static_cast<int>(Roles::ExpandedRole), "expanded" },
    };
}

std::unique_ptr<QQuickWindow> loadSwitcherWindow(QQmlEngine& engine, SwitcherController& switcher)
{
    auto component = QQmlComponent(&engine);
    component.loadFromModule("Contour.Ui", "Switcher");
    if (!component.isReady())
    {
        errorLog()("Could not load the switcher: {}", component.errorString().toStdString());
        return nullptr;
    }
    auto object = std::unique_ptr<QObject>(component.createWithInitialProperties(
        QVariantMap { { QStringLiteral("switcher"), QVariant::fromValue(&switcher) } }));
    if (qobject_cast<QQuickWindow*>(object.get()) == nullptr)
    {
        errorLog()("The switcher did not build a window: {}",
                   object ? object->metaObject()->className() : component.errorString().toStdString());
        return nullptr;
    }
    return std::unique_ptr<QQuickWindow>(static_cast<QQuickWindow*>(object.release()));
}

} // namespace contour::window
