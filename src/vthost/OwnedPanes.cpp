// SPDX-License-Identifier: Apache-2.0
#include <vthost/OwnedPanes.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <vthost/Logging.hpp>
#include <vthost/RevealOnLoop.hpp>

namespace vthost
{

std::optional<vtbackend::ProgramState> paneStatusOf(std::span<vtbackend::ProgramStatusRecord const> records)
{
    for (auto const source: PaneStatusSources)
        if (auto const found = std::ranges::find(records, source, &vtbackend::ProgramStatusRecord::id);
            found != records.end())
            return found->state;
    return std::nullopt;
}

OwnedPanes::OwnedPanes(OwnedPanesServices services):
    _services { std::move(services) }, _heldDirectories { _services.host.heldDirectorySpawn() }
{
    // Every recorded pane is open as far as the files tell; which still exist is decided by reconcile(). A
    // file holding more than an extension may own is cut at the bound, as open() would have refused the rest.
    for (auto const& [extension, records]: _services.store.loadAll())
    {
        if (records.size() > MaxOwnedPanesPerExtension)
            errorLog()("the owned panes of {} list {}; the first {} are kept",
                       extension,
                       records.size(),
                       MaxOwnedPanesPerExtension);
        for (auto const& record: records | std::views::take(MaxOwnedPanesPerExtension))
            _entries.push_back(Entry { .extension = extension,
                                       .pane = record.pane,
                                       .tag = record.tag,
                                       .state = OwnedPaneState::Open });
    }
}

void OwnedPanes::askedTooEarly(std::string_view extension) const
{
    if (!_askedTooEarly.contains(extension))
        _askedTooEarly.emplace(extension);
}

std::expected<std::string, OwnedPaneError> OwnedPanes::open(OwnedPaneSpawn spawn)
{
    // Refused before any pane exists: a program that cannot start in its directory here must not look like
    // an agent that never reported.
    if (_heldDirectories.load(std::memory_order_acquire) == vtpty::HeldDirectorySpawn::Unsupported)
        return std::unexpected(OwnedPaneError::NotSupported);
    if (spawn.directory && spawn.directory->isRemoved())
        return std::unexpected(OwnedPaneError::DirectoryGone);
    // A live upgrade closes what it cannot carry; a pane opened now could not be created before it.
    if (_services.admission.current() == ServedAdmission::Closed)
        return std::unexpected(OwnedPaneError::Upgrading);
    // Answered now, by name: a program that is not there would only fail the spawn, which tells nobody why.
    if (!_services.host.findsProgram(spawn))
        return std::unexpected(OwnedPaneError::ProgramMissing);
    auto pane = vtcontrol::Uuid {};
    {
        auto const lock = std::lock_guard { _mutex };
        if (_reconciliation == Reconciliation::Unreconciled)
        {
            askedTooEarly(spawn.extension);
            return std::unexpected(OwnedPaneError::NotReconciled);
        }
        if (std::ranges::count(_entries, spawn.extension, &Entry::extension)
            >= static_cast<std::ptrdiff_t>(MaxOwnedPanesPerExtension))
            return std::unexpected(OwnedPaneError::TooMany);
        pane = _services.ids.next();
        // Listed from now on, pending: an extension that lists its panes finds the one it just opened.
        _entries.push_back(Entry { .extension = spawn.extension, .pane = pane, .tag = spawn.tag });
    }
    _services.post([this, pane, spawn = std::move(spawn)] { create(pane, spawn); });
    return vtcontrol::toString(pane);
}

std::optional<vtworkspace::WorkspaceId> OwnedPanes::workspaceFor(
    std::string_view extension, std::optional<std::string> const& nearTab) const
{
    // Beside the dashboard it was dispatched from; else where the user looked last; else anywhere. A
    // dashboard that closed, or another extension's tab, is no reason to fail.
    if (nearTab)
        if (auto const workspace = _services.servedTabs.workspaceOfTab(extension, *nearTab))
            return workspace;
    if (auto const focused = _services.workspaces.mostRecentlyFocused())
        if (auto const workspace = _services.workspaces.selectedBy(*focused))
            return workspace;
    return _services.host.firstWorkspace();
}

void OwnedPanes::create(vtcontrol::Uuid pane, OwnedPaneSpawn const& spawn)
{
    auto cancelled = false;
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_entries, pane, &Entry::pane);
        if (found == _entries.end())
            return;
        if (found->state == OwnedPaneState::Cancelled)
        {
            // Closed before it was created: nothing is.
            _entries.erase(found);
            cancelled = true;
        }
    }
    if (cancelled)
    {
        _services.panesChanged(spawn.extension);
        return;
    }

    // An upgrade that began after the pane-open closes what it cannot carry: nothing is created meanwhile,
    // and the run fails as a pane-open refused for it would. A profile reload that took held directories
    // away, or a directory removed since, fails it as well.
    auto session = std::optional<vtworkspace::SessionId> {};
    auto const admitted = _services.admission.current() == ServedAdmission::Open
                          && _services.host.heldDirectorySpawn() == vtpty::HeldDirectorySpawn::Supported
                          && !(spawn.directory && spawn.directory->isRemoved());
    if (auto const workspace = admitted ? workspaceFor(spawn.extension, spawn.nearTab) : std::nullopt)
        session = _services.host.createOwnedTab(spawn, pane, *workspace); // opened out of focus

    auto closeNow = false;
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_entries, pane, &Entry::pane);
        if (found == _entries.end())
            closeNow = session.has_value();
        else if (!session)
            _entries.erase(found);
        else
        {
            found->session = session;
            if (found->state == OwnedPaneState::Cancelled)
                closeNow = true; // Closed while it was being created: listed until its session ended.
            else
                found->state = OwnedPaneState::Open;
        }
    }
    if (!session)
        sessionLog()(
            "the agent pane {} of {} could not be created", vtcontrol::toString(pane), spawn.extension);
    else
    {
        // Saved even when it closes at once: hosted until its session ended, it is carried as the agent pane
        // it is by an upgrade that captures it meanwhile.
        save(spawn.extension);
        if (closeNow)
            _services.host.closeOwnedSession(*session);
    }
    _services.panesChanged(spawn.extension);
}

std::expected<void, OwnedPaneError> OwnedPanes::close(std::string_view extension, std::string_view pane)
{
    auto const id = vtcontrol::parseUuid(pane);
    auto session = vtworkspace::SessionId {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find_if(_entries, [&](Entry const& entry) {
            return id && entry.pane == *id && entry.extension == extension;
        });
        if (found == _entries.end())
            return std::unexpected(OwnedPaneError::NotOwned);
        if (found->state == OwnedPaneState::Cancelled || found->state == OwnedPaneState::Closing)
            return {};
        if (found->state == OwnedPaneState::Pending)
        {
            found->state = OwnedPaneState::Cancelled;
            return {};
        }
        if (!found->session)
            return std::unexpected(OwnedPaneError::NotReconciled);
        found->state = OwnedPaneState::Closing;
        session = *found->session;
    }
    // Listed until its session ended (sessionEnded()), as every other pane that goes.
    _services.post([this, session] { _services.host.closeOwnedSession(session); });
    return {};
}

std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> OwnedPanes::list(std::string_view extension) const
{
    auto const lock = std::lock_guard { _mutex };
    if (_reconciliation == Reconciliation::Unreconciled)
    {
        askedTooEarly(extension);
        return std::unexpected(OwnedPaneError::NotReconciled);
    }
    auto listed = std::vector<OwnedPaneInfo> {};
    for (auto const& entry: _entries)
        if (entry.extension == extension)
            listed.push_back(OwnedPaneInfo { .pane = vtcontrol::toString(entry.pane),
                                             .tag = entry.tag,
                                             .status = entry.status,
                                             .program = entry.program,
                                             .state = entry.state });
    return listed;
}

std::expected<void, OwnedPaneError> OwnedPanes::reveal(std::string_view extension,
                                                       std::string_view pane,
                                                       Origin origin)
{
    auto const id = vtcontrol::parseUuid(pane);
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find_if(_entries, [&](Entry const& entry) {
            return id && entry.pane == *id && entry.extension == extension;
        });
        if (found == _entries.end())
            return std::unexpected(OwnedPaneError::NotOwned);
        switch (found->state)
        {
            case OwnedPaneState::Cancelled:
            case OwnedPaneState::Closing: return std::unexpected(OwnedPaneError::Gone);
            case OwnedPaneState::Open:
                // Read from the files, and not checked yet against what the host restored.
                if (!found->session)
                    return std::unexpected(OwnedPaneError::NotReconciled);
                break;
            case OwnedPaneState::Pending: break; // its creation, posted earlier, runs first
        }
    }
    // The origin's window, if the user invoked the action behind it just now and no reveal claimed it yet;
    // else that of a key or click into one of the extension's served tabs just now, which is what asked for
    // it (a dashboard's input carries no origin). Without either no user action is behind the reveal -- an
    // origin never handed to it (0, made up, another extension's), one gone stale, one a reveal claimed
    // already, one whose window detached: an extension never raises a window on its own, and never long after
    // the user acted, when what they type would follow the window it raised.
    auto const connection = _services.origins.claimReveal(extension, origin).or_else([&] {
        return _services.servedTabs.recentInputFrom(extension);
    });
    auto const workspace = connection ? _services.workspaces.selectedBy(*connection) : std::nullopt;
    if (!workspace)
        return std::unexpected(OwnedPaneError::Gone);
    // Looked up on the loop: a pending pane's session exists only once its creation ran there.
    _services.post([this, pane = *id, connection = *connection, workspace = *workspace] {
        auto session = std::optional<vtworkspace::SessionId> {};
        {
            auto const lock = std::lock_guard { _mutex };
            if (auto const found = std::ranges::find(_entries, pane, &Entry::pane); found != _entries.end())
                session = found->session;
        }
        if (session)
            revealSession(_services.tabHost, *session, connection, workspace);
    });
    return {};
}

void OwnedPanes::reconcile(std::span<HostedPane const> hosted)
{
    auto had = std::set<std::string, std::less<>> {};
    {
        auto const lock = std::lock_guard { _mutex };
        if (_reconciliation == Reconciliation::Reconciled)
            return;
        for (auto const& entry: _entries)
            had.insert(entry.extension);
        // An extension told to retry hears that it may, whether or not it has panes.
        had.merge(_askedTooEarly);
        _askedTooEarly.clear();
        // A pane the host does not hold went with the restart: a cold one restores none.
        for (auto& entry: _entries)
            if (auto const found = std::ranges::find(hosted, entry.pane, &HostedPane::pane);
                found != hosted.end())
            {
                entry.session = found->session;
                entry.status = found->status;
            }
        std::erase_if(_entries, [](Entry const& entry) { return !entry.session.has_value(); });
        _reconciliation = Reconciliation::Reconciled;
    }
    for (auto const& extension: had)
    {
        save(extension);
        _services.panesChanged(extension);
    }
}

void OwnedPanes::sessionEnded(vtworkspace::SessionId session)
{
    auto extension = std::string {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = entryOf(session);
        if (found == _entries.end())
            return;
        extension = found->extension;
        _entries.erase(found);
    }
    save(extension);
    _services.panesChanged(extension);
}

void OwnedPanes::programExited(vtworkspace::SessionId session)
{
    auto extension = std::string {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = entryOf(session);
        if (found == _entries.end() || found->program == PaneLiveness::Gone)
            return;
        found->program = PaneLiveness::Gone;
        extension = found->extension;
    }
    _services.panesChanged(extension);
}

void OwnedPanes::statusChanged(vtworkspace::SessionId session, std::optional<vtbackend::ProgramState> status)
{
    auto extension = std::string {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = entryOf(session);
        if (found == _entries.end() || found->status == status)
            return;
        found->status = status;
        extension = found->extension;
    }
    _services.panesChanged(extension);
}

void OwnedPanes::heldDirectoriesChanged()
{
    _heldDirectories.store(_services.host.heldDirectorySpawn(), std::memory_order_release);
}

bool OwnedPanes::owns(vtworkspace::SessionId session) const
{
    auto const lock = std::lock_guard { _mutex };
    return std::ranges::contains(_entries, std::optional { session }, &Entry::session);
}

bool OwnedPanes::ownsPane(vtcontrol::Uuid const& pane) const
{
    auto const lock = std::lock_guard { _mutex };
    return std::ranges::contains(_entries, pane, &Entry::pane);
}

void OwnedPanes::save(std::string_view extension)
{
    // Still hosted, so carried by an upgrade before its session ended: open, closing, or closed while it was
    // being created and ending now.
    auto const isHosted = [](Entry const& entry) {
        return entry.state == OwnedPaneState::Open || entry.state == OwnedPaneState::Closing
               || (entry.state == OwnedPaneState::Cancelled && entry.session.has_value());
    };
    auto records = std::vector<OwnedPaneRecord> {};
    {
        auto const lock = std::lock_guard { _mutex };
        for (auto const& entry: _entries)
            if (entry.extension == extension && isHosted(entry))
                records.push_back(OwnedPaneRecord { .pane = entry.pane, .tag = entry.tag });
    }
    std::ignore = _services.store.save(extension, records);
}

std::vector<OwnedPanes::Entry>::iterator OwnedPanes::entryOf(vtworkspace::SessionId session)
{
    return std::ranges::find(_entries, std::optional { session }, &Entry::session);
}

} // namespace vthost
