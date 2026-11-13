// SPDX-License-Identifier: Apache-2.0
#include <contour/command/MenuTree.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/SwitcherController.hpp>
#include <contour/window/Utf16Offsets.hpp>

#include <QtCore/QPointer>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace contour::window
{

namespace
{
    using vthost::proto::SwitchDaemonKind;

    /// @return A local daemon's address, with @p workspace.
    [[nodiscard]] session::SwitchTarget labelled(std::string label, session::WorkspaceTarget workspace = {})
    {
        return session::SwitchTarget { .kind = SwitchDaemonKind::Label,
                                       .daemon = std::move(label),
                                       .workspace = std::move(workspace) };
    }

    /// @return The daemon @p target names, with no workspace.
    [[nodiscard]] session::SwitchTarget daemonOf(session::SwitchTarget const& target)
    {
        return session::SwitchTarget { .kind = target.kind, .daemon = target.daemon, .workspace = {} };
    }

    /// @return Whether @p daemon is the one @p address names.
    [[nodiscard]] bool isAt(SwitcherDaemon const& daemon, session::SwitchTarget const& address)
    {
        return daemon.address.kind == address.kind && daemon.address.daemon == address.daemon;
    }

    /// One Qt key the switcher handles, and what the key table calls it.
    struct QtKeyRow
    {
        int qtKey = 0;
        SwitcherKey key = SwitcherKey::Other;
    };

    constexpr auto QtKeyRows = std::array {
        QtKeyRow { .qtKey = Qt::Key_Up, .key = SwitcherKey::Up },
        QtKeyRow { .qtKey = Qt::Key_Down, .key = SwitcherKey::Down },
        QtKeyRow { .qtKey = Qt::Key_Home, .key = SwitcherKey::Home },
        QtKeyRow { .qtKey = Qt::Key_End, .key = SwitcherKey::End },
        QtKeyRow { .qtKey = Qt::Key_Return, .key = SwitcherKey::Return },
        QtKeyRow { .qtKey = Qt::Key_Enter, .key = SwitcherKey::Return },
        QtKeyRow { .qtKey = Qt::Key_Right, .key = SwitcherKey::Right },
        QtKeyRow { .qtKey = Qt::Key_Left, .key = SwitcherKey::Left },
        QtKeyRow { .qtKey = Qt::Key_Backspace, .key = SwitcherKey::Backspace },
        QtKeyRow { .qtKey = Qt::Key_Escape, .key = SwitcherKey::Escape },
        QtKeyRow { .qtKey = Qt::Key_Menu, .key = SwitcherKey::Menu },
        QtKeyRow { .qtKey = Qt::Key_F10, .key = SwitcherKey::F10 },
        QtKeyRow { .qtKey = Qt::Key_J, .key = SwitcherKey::J },
        QtKeyRow { .qtKey = Qt::Key_K, .key = SwitcherKey::K },
    };

    [[nodiscard]] SwitcherKey switcherKeyOf(int qtKey) noexcept
    {
        auto const found = std::ranges::find(QtKeyRows, qtKey, &QtKeyRow::qtKey);
        return found != QtKeyRows.end() ? found->key : SwitcherKey::Other;
    }

    /// One Qt modifier, and its bit in HeldModifier.
    struct QtModifierRow
    {
        Qt::KeyboardModifier qt = Qt::NoModifier;
        HeldModifier held = HeldModifier::None;
    };

    // Qt's names: ControlModifier is ⌘ on macOS and MetaModifier the Ctrl key there (HeldModifier).
    constexpr auto QtModifierRows = std::array {
        QtModifierRow { .qt = Qt::ShiftModifier, .held = HeldModifier::Shift },
        QtModifierRow { .qt = Qt::ControlModifier, .held = HeldModifier::Control },
        QtModifierRow { .qt = Qt::AltModifier, .held = HeldModifier::Alt },
        QtModifierRow { .qt = Qt::MetaModifier, .held = HeldModifier::Meta },
    };

    [[nodiscard]] HeldModifier heldOf(int modifiers) noexcept
    {
        auto held = HeldModifier::None;
        for (auto const& row: QtModifierRows)
            if ((modifiers & row.qt) != 0)
                held = held | row.held;
        return held;
    }

    [[nodiscard]] QVariantList utf16Matches(SwitcherRow const& row)
    {
        auto list = QVariantList {};
        for (auto const index: byteOffsetsToUtf16(row.title, row.titleMatches))
            list.push_back(index);
        return list;
    }

    /// @return Whether @p a and @p b are the same rows, in the same order, under the same headers: a rebuild
    ///         that only changes what they say.
    [[nodiscard]] bool sameRows(std::span<SwitcherRow const> a, std::span<SwitcherRow const> b)
    {
        return std::ranges::equal(a, b, [](SwitcherRow const& x, SwitcherRow const& y) {
            return switcherRowIdentity(x) == switcherRowIdentity(y) && x.groupTitle == y.groupTitle;
        });
    }

    /// @return @p sets for QML.
    [[nodiscard]] QStringList footerHintSetsOf(std::vector<std::string> const& sets)
    {
        auto list = QStringList {};
        for (auto const& each: sets)
            list.push_back(QString::fromStdString(each));
        return list;
    }
} // namespace

SwitcherController::SwitcherController(SwitcherServices services, QObject* parent):
    QAbstractListModel(parent), _services(std::move(services))
{
}

SwitcherController::~SwitcherController() = default;

SwitcherInputs SwitcherController::inputs() const
{
    return SwitcherInputs { .daemons = _daemons,
                            .windowDaemon = _windowDaemon,
                            .discovery = _discovery,
                            .showSwitcherShortcut = _showSwitcherShortcut };
}

std::size_t SwitcherController::indexOf(session::SwitchTarget const& address) const
{
    return static_cast<std::size_t>(
        std::distance(_daemons.begin(),
                      std::ranges::find_if(_daemons, [&](auto const& each) { return isAt(each, address); })));
}

SwitcherView SwitcherController::view() const
{
    return SwitcherView { .level = _level,
                          .daemon = indexOf(_level == SwitcherLevel::Daemon ? _levelAddress : _scopeAddress),
                          .origin = _opening.origin,
                          .scope = _scope,
                          .filter = _filterText };
}

SwitcherKeyContext SwitcherController::keyContext(int caret) const
{
    auto const length = static_cast<int>(_filter.size());
    auto const at = [&] {
        if (length == 0)
            return CaretAt::StartAndEnd;
        if (caret <= 0)
            return CaretAt::Start;
        return caret >= length ? CaretAt::End : CaretAt::Inside;
    }();
    auto const* const selected = rowAt(_current);
    return SwitcherKeyContext { .filter = length == 0 ? FilterText::Empty : FilterText::Typed,
                                .caret = at,
                                .level = _level,
                                .origin = _opening.origin,
                                .scope = _scope,
                                .selected =
                                    selected != nullptr ? std::optional { selected->kind } : std::nullopt };
}

SwitcherRow const* SwitcherController::rowAt(int row) const noexcept
{
    if (row < 0 || static_cast<std::size_t>(row) >= _rows.size())
        return nullptr;
    return &_rows[static_cast<std::size_t>(row)];
}

SwitcherWorkspace const* SwitcherController::workspaceOf(SwitcherRow const& row) const
{
    if (row.kind != SwitcherRowKind::Workspace || row.daemon >= _daemons.size())
        return nullptr;
    auto const& listed = _daemons[row.daemon].workspaces;
    auto const found = std::ranges::find_if(listed, [&](SwitcherWorkspace const& each) {
        return row.target.workspace == session::WorkspaceTarget { each.ref };
    });
    return found != listed.end() ? &*found : nullptr;
}

std::optional<SwitcherRowIdentity> SwitcherController::selectedIdentity() const
{
    auto const* const row = rowAt(_current);
    return row != nullptr ? std::optional { switcherRowIdentity(*row) } : std::nullopt;
}

void SwitcherController::open(SwitcherOpening opening)
{
    ++_openings;
    _opening = std::move(opening);
    _filter.clear();
    _filterText.clear();
    _selectionOrigin = SwitcherSelectionOrigin::Opening;
    _returns.clear();
    _pendingListings.clear();
    _labelFailure.clear();
    _rowMenu = {};
    _showSwitcherShortcut =
        _services.showSwitcherShortcut ? _services.showSwitcherShortcut() : std::string {};
    dropStart();

    // The window's own daemon is not listed: its workspaces are its sidebar's.
    auto const& own = _opening.windowDaemon;
    auto ownDaemon = SwitcherDaemon { .address = daemonOf(own),
                                      .workspaceCount = _opening.windowWorkspaces.size(),
                                      .tag = {},
                                      .listing = SwitcherListing::Listed,
                                      .failure = {},
                                      .workspaces = _opening.windowWorkspaces };
    _daemons.clear();
    setDiscovery(SwitcherDiscovery::Settled);
    if (_opening.pick)
    {
        // A pick: the one daemon a switch asked about, listed already; the window's own beside it, for the
        // rows of no daemon's.
        _scope = SwitcherScope::OneDaemon;
        if (isAt(*_opening.pick, own))
            _daemons.push_back(*_opening.pick);
        else
            _daemons = { std::move(ownDaemon), *_opening.pick };
        _windowDaemon = 0;
        _scopeAddress = _daemons.back().address;
    }
    else
    {
        // The saved hosts after the local daemons; the window's own first, or, on a saved host, that host.
        // A window with no daemon lists its own workspaces and no saved host.
        _scope = _opening.scope;
        _daemons = _services.savedHosts && _scope != SwitcherScope::WindowOnly
                       ? _services.savedHosts()
                       : std::vector<SwitcherDaemon> {};
        if (auto const found =
                std::ranges::find_if(_daemons, [&](auto const& each) { return isAt(each, own); });
            found != _daemons.end())
        {
            found->listing = SwitcherListing::Listed;
            found->workspaces = _opening.windowWorkspaces;
        }
        else if (own.kind == SwitchDaemonKind::Host)
            _daemons.push_back(std::move(ownDaemon));
        else
            _daemons.insert(_daemons.begin(), std::move(ownDaemon));
        _windowDaemon = static_cast<std::size_t>(
            std::distance(_daemons.begin(),
                          std::ranges::find_if(_daemons, [&](auto const& each) { return isAt(each, own); })));
        _scopeAddress = daemonOf(own);
        // The chip's dropdown looks for the others once it is widened.
        if (_scope == SwitcherScope::Everything)
            discoverDaemons();
    }
    setLevel(_opening.prompt == SwitcherPrompt::NewDaemon ? SwitcherLevel::NewDaemon : SwitcherLevel::Top);
    rebuild(std::nullopt);
    emit filterChanged();
}

void SwitcherController::updateWindowWorkspaces(std::vector<SwitcherWorkspace> workspaces)
{
    if (_windowDaemon >= _daemons.size())
        return;
    auto& own = _daemons[_windowDaemon];
    own.workspaceCount = workspaces.size();
    own.workspaces = std::move(workspaces);
    rebuild(selectedIdentity());
}

void SwitcherController::discoverDaemons()
{
    setDiscovery(SwitcherDiscovery::Running);
    // A reopening (the workspace chip clicked again) while one runs takes over its answer rather than start a
    // second: what the daemons are does not depend on the opening, and each discovery holds a worker.
    _discoveryFor = _openings;
    if (_discoveryJob == DiscoveryJob::Running || !_services.discovery)
        return;
    _discoveryJob = DiscoveryJob::Running;
    // Off the GUI thread: a socket that accepts and never answers holds the probe for its whole timeout. The
    // worker never touches this object; its answer is posted back, and the controller outlives its workers.
    _jobs.run([finder = _services.discovery] { return finder->discover(); },
              [this](std::vector<cli::DiscoveredDaemon> const& found) {
                  _discoveryJob = DiscoveryJob::Idle;
                  if (_discoveryFor != _openings)
                      return; // the opening it served has ended (chosen, cancelled, or a pick opened since)
                  mergeLocalDaemons(found);
              });
}

void SwitcherController::mergeLocalDaemons(std::vector<cli::DiscoveredDaemon> const& found)
{
    auto const keep = selectedIdentity();
    auto const own = _daemons[_windowDaemon].address;
    auto merged = std::vector<SwitcherDaemon> {};
    // The window's own daemon first (an unnameable one, or its label), so its workspaces lead every tie;
    // then every other label discovery found, in its order (`default` first); the saved hosts last.
    for (auto& daemon: _daemons)
        if (daemon.address.kind == SwitchDaemonKind::Same
            || (isAt(daemon, own) && own.kind == SwitchDaemonKind::Label))
        {
            auto taken = std::exchange(daemon, SwitcherDaemon {});
            merged.push_back(std::move(taken));
        }
    for (auto const& each: found)
    {
        auto const address = labelled(each.label);
        if (own.kind == SwitchDaemonKind::Label && own.daemon == each.label)
            continue; // listed first already
        if (auto const existing =
                std::ranges::find_if(_daemons, [&](auto const& known) { return isAt(known, address); });
            existing != _daemons.end())
        {
            if (existing->listing != SwitcherListing::Listed)
                existing->workspaceCount = each.workspaces;
            merged.push_back(std::exchange(*existing, SwitcherDaemon {}));
        }
        else
            merged.push_back(SwitcherDaemon { .address = address,
                                              .workspaceCount = each.workspaces,
                                              .tag = {},
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
    // The scope and the level hold their daemons by address: found again wherever the merge put them.
    _windowDaemon = indexOf(own);
    setDiscovery(SwitcherDiscovery::Settled);
    // Every local daemon is listed now, so typing finds their workspaces; a saved host waits to be drilled
    // into (connecting may ask for credentials, which no keystroke may trigger).
    for (auto const index: std::views::iota(std::size_t { 0 }, _daemons.size()))
        if (index != _windowDaemon && _daemons[index].address.kind == SwitchDaemonKind::Label)
            listDaemon(index);
    rebuild(keep);
}

void SwitcherController::listDaemon(std::size_t index)
{
    auto& daemon = _daemons[index];
    if (daemon.listing == SwitcherListing::Listing || !_services.list)
        return;
    daemon.listing = SwitcherListing::Listing;
    daemon.failure.clear();
    auto const serial = ++_listings;
    std::erase_if(_pendingListings, [&](auto const& each) { return each.first == daemon.address; });
    _pendingListings.emplace_back(daemon.address, serial);
    // Answered later on the GUI thread, found again by its address: the list may have changed meanwhile, and
    // the controller may be gone.
    _services.list(
        _opening.window,
        daemon.address,
        [self = QPointer { this }, opening = _openings, serial, address = daemon.address](auto listed) {
            if (!self || opening != self->_openings)
                return;
            // Backed out of before it answered (a saved host): dropped.
            if (std::erase_if(
                    self->_pendingListings,
                    [&](auto const& each) { return each.first == address && each.second == serial; })
                == 0)
                return;
            auto const found =
                std::ranges::find_if(self->_daemons, [&](auto const& each) { return isAt(each, address); });
            if (found == self->_daemons.end())
                return;
            if (listed)
            {
                found->listing = SwitcherListing::Listed;
                found->workspaces = *std::move(listed);
                found->workspaceCount = found->workspaces.size();
            }
            else
            {
                found->listing = SwitcherListing::Failed;
                found->failure = listed.error();
            }
            self->rebuild(self->selectedIdentity());
        });
}

void SwitcherController::rebuild(std::optional<SwitcherRowIdentity> const& keep)
{
    auto rows = switcherRows(inputs(), view());
    if (sameRows(rows, _rows) && !rows.empty())
    {
        // The same rows saying something new (a status, a count): updated in place, so nothing re-animates.
        _rows = std::move(rows);
        emit dataChanged(index(0), index(static_cast<int>(_rows.size()) - 1));
    }
    else
    {
        beginResetModel();
        _rows = std::move(rows);
        endResetModel();
    }
    // Until the user takes the selection, it is the popup's: it follows the list as it fills.
    auto const next = [&] {
        if (_rows.empty())
            return -1;
        if (_selectionOrigin == SwitcherSelectionOrigin::Opening)
            return static_cast<int>(switcherInitialSelection(_rows));
        return static_cast<int>(switcherSelection(_rows, keep));
    }();
    if (std::exchange(_current, next) != next)
        emit currentIndexChanged();
    updatePresentation();
}

void SwitcherController::updatePresentation()
{
    auto const currentView = view();
    auto const currentInputs = inputs();
    auto const daemonName = [&](std::size_t index) {
        return index < _daemons.size() ? switcherDaemonName(_daemons[index].address) : std::string {};
    };
    auto const label =
        _level == SwitcherLevel::NewDaemon
            ? switcherLabelState(_filterText, switcherRunningLabels(currentInputs), _services.style)
            : SwitcherLabelState {};
    if (std::exchange(_labelState, label) != label)
        emit labelChanged();

    auto const* const row = rowAt(_current);
    auto const subject = [&] {
        if (row == nullptr)
            return SwitcherHintSubject::None;
        return row->kind == SwitcherRowKind::LabelAction ? _labelState.subject : switcherHintSubject(*row);
    }();
    auto const matchesSomething = std::ranges::any_of(_rows, [](SwitcherRow const& each) {
        return each.kind == SwitcherRowKind::Workspace || each.kind == SwitcherRowKind::Daemon;
    });
    auto const title =
        QString::fromStdString(switcherLevelTitle(currentView, daemonName(indexOf(_levelAddress))));
    auto next = Presentation {
        .fieldName = fieldNameOf(title),
        .levelTitle = title,
        .hintText = QString::fromStdString(switcherHintText(currentView, daemonName(currentView.daemon))),
        .emptyText = !_filterText.empty() && _level != SwitcherLevel::NewDaemon && !matchesSomething
                         ? QString::fromStdString(switcherEmptyText(_filterText))
                         : QString {},
        .footerDescription =
            row != nullptr
                ? QString::fromStdString(switcherDescription(*row, currentInputs, currentView, _opening.mode))
                : QString {},
        .footerHintSets = footerHintSetsOf(switcherFooterHintSets(
            subject, switcherEscapeCommand(keyContext(0)), _services.style, _opening.mode)),
    };
    if (std::exchange(_presentation, std::move(next)) != _presentation)
        emit presentationChanged();
}

void SwitcherController::replaceFilter(std::string text)
{
    // Another label than the one a start runs for: that start is no longer what was asked for.
    if (text != _filterText)
        dropStart();
    _filterText = std::move(text);
    _filter = QString::fromStdString(_filterText);
    _labelFailure.clear();
    emit filterChanged();
}

void SwitcherController::setFilter(QString const& filter)
{
    if (_filter == filter)
        return;
    replaceFilter(filter.toStdString());
    // Typing picks the best match afresh.
    _selectionOrigin = SwitcherSelectionOrigin::Opening;
    rebuild(std::nullopt);
}

void SwitcherController::setCurrentIndex(int index)
{
    auto const clamped = _rows.empty() ? -1 : std::clamp(index, 0, static_cast<int>(_rows.size()) - 1);
    _selectionOrigin = SwitcherSelectionOrigin::User;
    if (_current == clamped)
        return;
    _current = clamped;
    emit currentIndexChanged();
    updatePresentation();
}

void SwitcherController::step(int delta)
{
    setCurrentIndex(_current + delta);
}

bool SwitcherController::handleKey(int key, int modifiers, int caret)
{
    // The keypad's Enter is Return: its modifier tells nothing apart here.
    auto const modifier = switcherKeyModifierOf(heldOf(modifiers & ~static_cast<int>(Qt::KeypadModifier)));
    auto const command = switcherKeyCommand(switcherKeyOf(key), modifier, keyContext(caret));
    switch (command)
    {
        case SwitcherCommand::None: return false;
        case SwitcherCommand::Previous: step(-1); break;
        case SwitcherCommand::Next: step(1); break;
        case SwitcherCommand::First: setCurrentIndex(0); break;
        case SwitcherCommand::Last: setCurrentIndex(static_cast<int>(_rows.size()) - 1); break;
        case SwitcherCommand::Activate: activate(_current); break;
        case SwitcherCommand::ActivateInNewWindow: activateInNewWindow(_current); break;
        case SwitcherCommand::DrillIn:
            if (auto const* const row = rowAt(_current))
                drillIn(*row);
            break;
        case SwitcherCommand::Back: back(); break;
        case SwitcherCommand::ClearFilter: setFilter({}); break;
        case SwitcherCommand::Close: cancel(); break;
        case SwitcherCommand::OpenRowMenu: emit rowMenuRequested(_current); break;
    }
    return true;
}

void SwitcherController::activate(int row)
{
    auto const* const found = rowAt(row);
    if (found == nullptr)
        return;
    auto const chosen = *found; // a choice may rebuild the rows
    switch (chosen.kind)
    {
        case SwitcherRowKind::Workspace: finishWith(chosen.target, _opening.mode); return;
        case SwitcherRowKind::Daemon:
        case SwitcherRowKind::NewDaemon:
        case SwitcherRowKind::AllDaemons: drillIn(chosen); return;
        case SwitcherRowKind::NewWorkspace: createWorkspace(chosen, _opening.mode); return;
        case SwitcherRowKind::LabelAction:
            submitLabel(_filter, static_cast<int>(std::to_underlying(_opening.mode)));
            return;
        case SwitcherRowKind::Discovering:
        case SwitcherRowKind::Connecting:
        case SwitcherRowKind::Failure: return;
    }
}

void SwitcherController::activateInNewWindow(int row)
{
    auto const* const found = rowAt(row);
    if (found == nullptr)
        return;
    auto const chosen = *found;
    if (switcherRowKindRule(chosen.kind).newWindow == OpensInNewWindow::No)
    {
        activate(row);
        return;
    }
    switch (chosen.kind)
    {
        case SwitcherRowKind::Workspace:
        case SwitcherRowKind::Daemon: finishWith(chosen.target, session::SwitchMode::Open); return;
        case SwitcherRowKind::NewWorkspace: createWorkspace(chosen, session::SwitchMode::Open); return;
        case SwitcherRowKind::LabelAction:
            submitLabel(_filter, static_cast<int>(std::to_underlying(session::SwitchMode::Open)));
            return;
        case SwitcherRowKind::NewDaemon:
        case SwitcherRowKind::AllDaemons:
        case SwitcherRowKind::Discovering:
        case SwitcherRowKind::Connecting:
        case SwitcherRowKind::Failure: activate(row); return;
    }
}

void SwitcherController::drillIn(SwitcherRow const& row)
{
    if (switcherRowKindRule(row.kind).drillsIn == DrillsIn::No)
        return;
    _returns.push_back(Return { .filter = _filterText, .selected = switcherRowIdentity(row) });
    switch (row.kind)
    {
        case SwitcherRowKind::Daemon:
            setLevel(SwitcherLevel::Daemon);
            _levelAddress = daemonOf(row.target);
            // A saved host connects now; a daemon whose listing failed is asked again.
            if (row.daemon < _daemons.size() && _daemons[row.daemon].listing != SwitcherListing::Listed
                && _daemons[row.daemon].listing != SwitcherListing::Listing)
                listDaemon(row.daemon);
            break;
        case SwitcherRowKind::NewDaemon: setLevel(SwitcherLevel::NewDaemon); break;
        case SwitcherRowKind::AllDaemons:
            _scope = SwitcherScope::Everything;
            // The chip's dropdown did not look for the other daemons as it opened.
            if (_discoveryFor != _openings)
                discoverDaemons();
            break;
        case SwitcherRowKind::Workspace:
        case SwitcherRowKind::NewWorkspace:
        case SwitcherRowKind::Discovering:
        case SwitcherRowKind::Connecting:
        case SwitcherRowKind::LabelAction:
        case SwitcherRowKind::Failure: break;
    }
    replaceFilter({});
    _selectionOrigin = SwitcherSelectionOrigin::Opening;
    rebuild(std::nullopt);
}

void SwitcherController::back()
{
    if (auto const level = indexOf(_levelAddress); _level == SwitcherLevel::Daemon && level < _daemons.size())
    {
        // A saved host still connecting is not waited for: its answer is dropped, and it reads as before.
        auto& left = _daemons[level];
        if (left.address.kind == SwitchDaemonKind::Host && left.listing == SwitcherListing::Listing)
        {
            std::erase_if(_pendingListings, [&](auto const& each) { return each.first == left.address; });
            left.listing = SwitcherListing::NotListed;
        }
    }
    // Leaving the New Daemon level leaves its start: it is dropped as a host's listing is.
    if (_level == SwitcherLevel::NewDaemon)
        dropStart();
    if (_level != SwitcherLevel::Top)
        setLevel(SwitcherLevel::Top);
    else if (_opening.origin == SwitcherOrigin::Chip && _scope == SwitcherScope::Everything)
        _scope = SwitcherScope::OneDaemon;
    else
        return;
    auto const restored = _returns.empty() ? Return {} : std::move(_returns.back());
    if (!_returns.empty())
        _returns.pop_back();
    replaceFilter(restored.filter);
    _selectionOrigin = restored.selected ? SwitcherSelectionOrigin::User : SwitcherSelectionOrigin::Opening;
    rebuild(restored.selected);
}

void SwitcherController::createWorkspace(SwitcherRow const& row, session::SwitchMode mode)
{
    if (_filterText.empty() || !_services.createWorkspace)
    {
        finishWith(row.target, mode);
        return;
    }
    ++_openings; // the opening ends here
    _services.createWorkspace(_opening.window, daemonOf(row.target), _filterText, mode);
    emit finished();
}

void SwitcherController::finishWith(session::SwitchTarget target, session::SwitchMode mode)
{
    // The opening ends here: whatever answers later for it is no longer wanted.
    ++_openings;
    _services.request(_opening.window, std::move(target), mode);
    emit finished();
}

QVariantList SwitcherController::rowMenu(int row)
{
    _rowMenu = {};
    auto const* const chosen = rowAt(row);
    if (chosen == nullptr || chosen->kind != SwitcherRowKind::Workspace)
        return {};
    auto const owner = chosen->daemon == _windowDaemon ? command::SwitcherMenuOwner::WindowDaemon
                                                       : command::SwitcherMenuOwner::OtherDaemon;
    auto const* const workspace = workspaceOf(*chosen);
    auto const id = workspace != nullptr ? workspace->id : uint64_t { 0 };
    auto const sidebarState = owner == command::SwitcherMenuOwner::WindowDaemon && _services.workspaceMenu
                                  ? _services.workspaceMenu(_opening.window, id)
                                  : command::WorkspaceMenuState {};
    auto const menu = command::buildSwitcherRowMenu(
        command::SwitcherRowMenuState { .name = chosen->title,
                                        .owner = owner,
                                        .workspace = sidebarState,
                                        .returnMode = _opening.mode == session::SwitchMode::Open
                                                          ? command::SwitcherMenuReturn::Open
                                                          : command::SwitcherMenuReturn::Switch,
                                        .shown = chosen->state == SwitcherWorkspaceState::ThisWindow
                                                     ? command::SwitcherMenuShown::ThisWindow
                                                     : command::SwitcherMenuShown::Elsewhere },
        _services.style);

    // The leading choices carry no action: they take the first actionIds, the sidebar's rows the rest.
    auto model = QVariantList {};
    auto entries = std::vector<command::ContextMenuEntry> {};
    for (auto const& each: menu)
    {
        if (auto const* const choice = std::get_if<command::SwitcherChoiceRow>(&each))
        {
            model.push_back(QVariantMap {
                { QStringLiteral("kind"), QStringLiteral("command") },
                { QStringLiteral("title"), QString::fromStdString(choice->title) },
                { QStringLiteral("enabled"), choice->availability == command::RowAvailability::Enabled },
                { QStringLiteral("checkable"), false },
                { QStringLiteral("checked"), false },
                { QStringLiteral("shortcut"), QString::fromStdString(choice->shortcut) },
                { QStringLiteral("actionId"), static_cast<int>(_rowMenu.choices.size()) },
            });
            _rowMenu.choices.push_back(choice->choice);
        }
        else
            entries.push_back(std::get<command::ContextMenuEntry>(each));
    }
    auto tree = command::buildMenuTree(entries, {}, _services.style);
    auto const offset = static_cast<int>(_rowMenu.choices.size());
    auto const shift = [offset](auto& self, QVariantList& rows) -> void {
        for (auto& each: rows)
        {
            auto map = each.toMap();
            if (map.contains(QStringLiteral("actionId")))
                map[QStringLiteral("actionId")] = map[QStringLiteral("actionId")].toInt() + offset;
            if (map.contains(QStringLiteral("children")))
            {
                auto children = map[QStringLiteral("children")].toList();
                self(self, children);
                map[QStringLiteral("children")] = children;
            }
            each = map;
        }
    };
    auto sidebarRows = toContextMenuModel(tree.nodes);
    shift(shift, sidebarRows);
    model.append(sidebarRows);
    _rowMenu.actions = std::move(tree.actions);
    _rowMenu.row = switcherRowIdentity(*chosen);
    return model;
}

void SwitcherController::triggerRowMenu(int /*row*/, int id)
{
    // The menu's pick has already closed the popup: when it finds nothing to act on, the opening ends.
    if (!_rowMenu.row || id < 0)
    {
        cancel();
        return;
    }
    // The row the menu was opened over, wherever a rebuild moved it since.
    auto const at = std::ranges::find(_rows, *_rowMenu.row, switcherRowIdentity);
    if (at == _rows.end())
    {
        cancel();
        return;
    }
    auto const row = static_cast<int>(std::distance(_rows.begin(), at));
    auto const choices = static_cast<int>(_rowMenu.choices.size());
    if (id < choices)
    {
        switch (_rowMenu.choices[static_cast<std::size_t>(id)])
        {
            case command::SwitcherMenuChoice::Switch: activate(row); break;
            case command::SwitcherMenuChoice::OpenInNewWindow: activateInNewWindow(row); break;
        }
        return;
    }
    auto const actionIndex = static_cast<std::size_t>(id - choices);
    if (actionIndex >= _rowMenu.actions.size() || !_services.runWorkspaceAction)
    {
        cancel();
        return;
    }
    auto const action = _rowMenu.actions[actionIndex];
    auto const* const workspace = workspaceOf(*at);
    if (workspace == nullptr)
    {
        cancel();
        return;
    }
    auto const workspaceId = workspace->id;
    // The sidebar's own path runs it (Rename… edits the sidebar's row): the popup is done.
    ++_openings;
    emit finished();
    _services.runWorkspaceAction(_opening.window, workspaceId, action);
}

void SwitcherController::checkLabel(QString const& label)
{
    setFilter(label);
}

void SwitcherController::submitLabel(QString const& label, int mode)
{
    if (_isBusy)
        return;
    auto const text = label.toStdString();
    auto const switchMode = std::cmp_equal(mode, std::to_underlying(session::SwitchMode::Open))
                                ? session::SwitchMode::Open
                                : session::SwitchMode::Switch;
    switch (switcherLabelVerdict(text, switcherRunningLabels(inputs())))
    {
        case LabelVerdict::Empty:
        case LabelVerdict::Invalid: return; // the verdict line says why
        case LabelVerdict::Running: finishWith(labelled(text, session::AnyWorkspace {}), switchMode); return;
        case LabelVerdict::Available: break;
    }
    if (!_services.startDaemon)
        return;
    setBusy(true);
    // Started off the GUI thread: the spawn waits for the daemon to serve. The worker never touches this
    // object; its answer is posted back (as discovery's is).
    auto const start = ++_starts;
    _jobs.run([job = _services.startDaemon(text)] { return job(); },
              [this, start, text, switchMode](std::expected<cli::NamedDaemonStart, std::string> result) {
                  finishStart(start, text, switchMode, std::move(result));
              });
}

void SwitcherController::finishStart(std::uint64_t start,
                                     std::string const& label,
                                     session::SwitchMode mode,
                                     std::expected<cli::NamedDaemonStart, std::string> result)
{
    // Dropped (cancelled, dismissed, left, relabelled, or the popup opened again): nothing is waiting for
    // this answer, and it must not switch.
    if (start != _starts)
        return;
    setBusy(false);
    if (!result)
    {
        _labelFailure = std::format("Could not start daemon “{}”: {}.", label, result.error());
        emit labelChanged();
        return;
    }
    // Started, or found running meanwhile: either way it serves now.
    finishWith(labelled(label, session::AnyWorkspace {}), mode);
}

void SwitcherController::abandon()
{
    ++_openings; // whatever is in flight is no longer wanted
    dropStart();
}

void SwitcherController::dropStart()
{
    ++_starts;
    setBusy(false);
}

void SwitcherController::setLevel(SwitcherLevel level)
{
    if (std::exchange(_level, level) != level)
        emit levelChanged();
}

void SwitcherController::setDiscovery(SwitcherDiscovery discovery)
{
    if (std::exchange(_discovery, discovery) != discovery)
        emit discoveringChanged();
}

QString SwitcherController::fieldNameOf(QString const& levelTitle) const
{
    switch (_level)
    {
        case SwitcherLevel::Top: return tr("Switch workspace or daemon");
        case SwitcherLevel::Daemon: return tr("Switch to a workspace of %1").arg(levelTitle);
        case SwitcherLevel::NewDaemon: return tr("Label of the new daemon");
    }
    return {};
}

QString SwitcherController::accessibleDescriptionOf(SwitcherRow const& row) const
{
    auto parts = QStringList {};
    switch (switcherRowKindRule(row.kind).role)
    {
        case SwitcherRowRole::Workspace: parts << tr("workspace"); break;
        case SwitcherRowRole::Daemon: parts << tr("daemon"); break;
        case SwitcherRowRole::Action: parts << tr("action"); break;
        case SwitcherRowRole::Progress: parts << tr("in progress"); break;
        case SwitcherRowRole::Error: parts << tr("error"); break;
    }
    if (auto const urgency = urgencyText(row.status.urgency); !urgency.empty())
        parts << QString::fromUtf8(urgency.data(), static_cast<qsizetype>(urgency.size()));
    switch (row.state)
    {
        case SwitcherWorkspaceState::Hidden: break;
        case SwitcherWorkspaceState::ShownElsewhere: parts << tr("in another window"); break;
        case SwitcherWorkspaceState::ThisWindow: parts << tr("current"); break;
    }
    // What the row's trailing detail draws -- git, the tabs, a failed command's reason and time -- which the
    // delegate keeps from assistive technology so it is not read twice.
    auto const detail =
        switcherDetail(row, inputs(), _services.now ? _services.now() : std::chrono::system_clock::now());
    if (auto const said = detail.text + detail.separator + detail.command + detail.tail; !said.empty())
        parts << QString::fromStdString(said);
    return parts.join(QStringLiteral(", "));
}

void SwitcherController::cancel()
{
    abandon();
    emit cancelled();
}

QString SwitcherController::labelCaption() const
{
    return tr("Label — names the daemon in the switcher, the sidebar and --label");
}

QString SwitcherController::labelCommandCaption() const
{
    return tr("Equivalent command");
}

int SwitcherController::labelVerdictGlyph() const noexcept
{
    if (!_labelFailure.empty())
        return static_cast<int>(std::to_underlying(TabBadgeKind::Error));
    switch (_labelState.verdict)
    {
        case LabelVerdict::Empty: return static_cast<int>(std::to_underlying(TabBadgeKind::None));
        case LabelVerdict::Available: return static_cast<int>(std::to_underlying(TabBadgeKind::Done));
        case LabelVerdict::Running:
        case LabelVerdict::Invalid: return static_cast<int>(std::to_underlying(TabBadgeKind::Error));
    }
    return 0;
}

QString SwitcherController::labelMessage() const
{
    return QString::fromStdString(_labelFailure.empty() ? _labelState.message : _labelFailure);
}

void SwitcherController::setBusy(bool busy)
{
    if (_isBusy == busy)
        return;
    _isBusy = busy;
    emit busyChanged();
    // The action row says the start is under way.
    if (!_rows.empty())
        emit dataChanged(index(0), index(static_cast<int>(_rows.size()) - 1));
}

QVariant SwitcherController::data(QModelIndex const& modelIndex, int role) const
{
    auto const* const row = rowAt(modelIndex.isValid() ? modelIndex.row() : -1);
    if (row == nullptr)
        return {};
    switch (static_cast<Roles>(role))
    {
        case Roles::TitleRole:
            if (row->kind == SwitcherRowKind::LabelAction && _isBusy)
                return tr("Starting the daemon…");
            return QString::fromStdString(row->title);
        case Roles::TitleMatchesRole: return utf16Matches(*row);
        case Roles::GroupTitleRole: return QString::fromStdString(row->groupTitle);
        case Roles::KindRole: return static_cast<int>(std::to_underlying(row->kind));
        case Roles::GlyphRole: {
            // A row waiting for an answer turns the Working ring (still under Reduce Motion, as everywhere).
            auto const waiting = switcherRowKindRule(row->kind).awaits == AwaitsAnswer::Yes
                                 || (row->kind == SwitcherRowKind::LabelAction && _isBusy);
            return static_cast<int>(std::to_underlying(waiting ? TabBadgeKind::Working : row->status.glyph));
        }
        case Roles::DetailTextRole:
        case Roles::DetailSeparatorRole:
        case Roles::DetailCommandRole:
        case Roles::DetailTailRole: {
            auto const detail = switcherDetail(
                *row, inputs(), _services.now ? _services.now() : std::chrono::system_clock::now());
            switch (static_cast<Roles>(role))
            {
                case Roles::DetailSeparatorRole: return QString::fromStdString(detail.separator);
                case Roles::DetailCommandRole: return QString::fromStdString(detail.command);
                case Roles::DetailTailRole: return QString::fromStdString(detail.tail);
                default: return QString::fromStdString(detail.text);
            }
        }
        case Roles::PillTextRole: return QString::fromStdString(row->pillText);
        case Roles::PillKindRole: return static_cast<int>(std::to_underlying(row->pill));
        case Roles::AccessibleDescriptionRole: return accessibleDescriptionOf(*row);
        case Roles::DrillsInRole: return switcherRowKindRule(row->kind).drillsIn == DrillsIn::Yes;
        case Roles::NewWindowNameRole:
            return tr("Open %1 in a new window").arg(QString::fromStdString(row->title));
        case Roles::OpensInNewWindowRole:
            return switcherRowKindRule(row->kind).button == NewWindowButton::Yes;
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
        { static_cast<int>(Roles::TitleMatchesRole), "titleMatches" },
        { static_cast<int>(Roles::GroupTitleRole), "groupTitle" },
        { static_cast<int>(Roles::KindRole), "kind" },
        { static_cast<int>(Roles::GlyphRole), "glyph" },
        { static_cast<int>(Roles::DetailTextRole), "detailText" },
        { static_cast<int>(Roles::DetailSeparatorRole), "detailSeparator" },
        { static_cast<int>(Roles::DetailCommandRole), "detailCommand" },
        { static_cast<int>(Roles::DetailTailRole), "detailTail" },
        { static_cast<int>(Roles::PillTextRole), "pillText" },
        { static_cast<int>(Roles::PillKindRole), "pillKind" },
        { static_cast<int>(Roles::AccessibleDescriptionRole), "accessibleDescription" },
        { static_cast<int>(Roles::DrillsInRole), "drillsIn" },
        { static_cast<int>(Roles::OpensInNewWindowRole), "opensInNewWindow" },
        { static_cast<int>(Roles::NewWindowNameRole), "newWindowName" },
    };
}

} // namespace contour::window
