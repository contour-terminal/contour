// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationHistory.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <algorithm>
#include <utility>

namespace contour::window
{

bool placeStillExists(SidebarTarget const& entry, std::span<DaemonSection const> sections)
{
    auto const section = std::ranges::find(sections, entry.daemon, &DaemonSection::daemon);
    if (section == sections.end() || section->instance != entry.instance)
        return false;
    return entry.tab ? findTab(section->directory, entry.workspace, *entry.tab).has_value()
                     : findWorkspace(section->directory, entry.workspace) != nullptr;
}

LocationHistory::LocationHistory(std::size_t capacity): _capacity { capacity }
{
}

void LocationHistory::landed(std::optional<SidebarTarget> from, SidebarTarget const& to, HistoryCause cause)
{
    // Whatever the landing, only an attention jump keeps a run going: JumpBack and every other kind end it.
    auto const run =
        std::exchange(_run, cause == HistoryCause::Attention ? AttentionRun::Open : AttentionRun::None);
    if (!from || landsWhereItWas(*from, to))
    {
        // Nothing moved: a run under way is not ended by it, nor one begun -- but JumpBack ends one wherever
        // it lands.
        if (cause != HistoryCause::Back)
            _run = run;
        return;
    }
    switch (cause)
    {
        case HistoryCause::Back: return;
        case HistoryCause::Attention:
            if (run == AttentionRun::Open)
                return;
            break;
        case HistoryCause::Jump: break;
    }
    // A place is a tab: the pane it showed is not kept.
    from->pane = std::nullopt;
    _entries.push_back(*std::move(from));
    if (_entries.size() > _capacity)
        _entries.pop_front();
}

bool LocationHistory::goesSomewhere(std::optional<SidebarTarget> const& current,
                                    SidebarTarget const& entry) noexcept
{
    return !current || !landsWhereItWas(*current, entry);
}

std::optional<SidebarTarget> LocationHistory::back(
    std::optional<SidebarTarget> const& current, std::function<bool(SidebarTarget const&)> const& stillExists)
{
    while (!_entries.empty())
    {
        auto entry = std::move(_entries.back());
        _entries.pop_back();
        if (goesSomewhere(current, entry) && stillExists(entry))
            return entry;
    }
    return std::nullopt;
}

bool LocationHistory::canGoBack(std::optional<SidebarTarget> const& current,
                                std::function<bool(SidebarTarget const&)> const& stillExists) const
{
    return std::ranges::any_of(_entries, [&](SidebarTarget const& entry) {
        return goesSomewhere(current, entry) && stillExists(entry);
    });
}

void LocationHistory::restore(SidebarTarget place)
{
    _entries.push_back(std::move(place));
    if (_entries.size() > _capacity)
        _entries.pop_front();
}

std::size_t LocationHistory::size() const noexcept
{
    return _entries.size();
}

} // namespace contour::window
