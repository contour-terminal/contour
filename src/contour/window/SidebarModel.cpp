// SPDX-License-Identifier: Apache-2.0
#include <contour/window/AttentionJump.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/SidebarDropPolicy.hpp>
#include <contour/window/SidebarModel.hpp>

#include <core/Utils.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <ranges>
#include <utility>

namespace contour::window
{

namespace
{
    using namespace std::chrono_literals;

    /// How often the running times are re-formatted: the most the sidebar ever repaints by itself.
    constexpr auto TickInterval = 1000ms;

    struct RoleName
    {
        SidebarModel::Roles role;
        char const* name;
    };

    constexpr auto RoleNames = std::array {
        RoleName { SidebarModel::KindRole, "kind" },
        RoleName { SidebarModel::DepthRole, "depth" },
        RoleName { SidebarModel::TitleRole, "title" },
        RoleName { SidebarModel::ReasonRole, "reason" },
        RoleName { SidebarModel::GitTextRole, "gitText" },
        RoleName { SidebarModel::TimeTextRole, "timeText" },
        RoleName { SidebarModel::TimeKindRole, "timeKind" },
        RoleName { SidebarModel::ProgressRole, "progress" },
        RoleName { SidebarModel::UrgencyRole, "urgency" },
        RoleName { SidebarModel::GlyphRole, "glyph" },
        RoleName { SidebarModel::ExpansionRole, "expansion" },
        RoleName { SidebarModel::SelectedRole, "selected" },
        RoleName { SidebarModel::TreeLinesRole, "treeLines" },
        RoleName { SidebarModel::AccessibleNameRole, "accessibleName" },
    };

    [[nodiscard]] QString qs(std::string const& text)
    {
        return QString::fromStdString(text);
    }

    [[nodiscard]] std::string_view kindWord(SidebarRowKind kind) noexcept
    {
        switch (kind)
        {
            case SidebarRowKind::Workspace: return "workspace";
            case SidebarRowKind::Tab: return "tab";
            case SidebarRowKind::Pane: return "pane";
        }
        return {};
    }

    /// The roles in which @p before and @p after differ: every one data() serves from what changed.
    [[nodiscard]] QList<int> changedRoles(SidebarRow const& before, SidebarRow const& after)
    {
        auto roles = QList<int> {};
        auto const note = [&](bool same, std::initializer_list<int> served) {
            if (!same)
                for (auto const role: served)
                    roles.push_back(role);
        };
        note(before.depth == after.depth, { SidebarModel::DepthRole });
        note(before.title == after.title,
             { SidebarModel::TitleRole, Qt::DisplayRole, Qt::AccessibleTextRole });
        note(before.reason == after.reason, { SidebarModel::ReasonRole });
        note(before.gitText == after.gitText, { SidebarModel::GitTextRole });
        note(before.time.text == after.time.text, { SidebarModel::TimeTextRole });
        note(before.time.kind == after.time.kind, { SidebarModel::TimeKindRole });
        note(before.time.percent == after.time.percent, { SidebarModel::ProgressRole });
        note(before.urgency == after.urgency, { SidebarModel::UrgencyRole });
        note(before.glyph == after.glyph, { SidebarModel::GlyphRole });
        note(before.expansion == after.expansion, { SidebarModel::ExpansionRole });
        note(before.selection == after.selection, { SidebarModel::SelectedRole });
        note(before.lines == after.lines, { SidebarModel::TreeLinesRole });
        // The spoken name folds in the status, the expansion and the selection; a running time is not in it.
        if (accessibleNameOf(before) != accessibleNameOf(after))
            for (auto const role: { static_cast<int>(SidebarModel::AccessibleNameRole),
                                    static_cast<int>(Qt::AccessibleTextRole) })
                if (!roles.contains(role))
                    roles.push_back(role);
        return roles;
    }

    [[nodiscard]] bool showsRunningTime(SidebarRow const& row) noexcept
    {
        return row.time.kind == TimeKind::Live || row.time.kind == TimeKind::Relative;
    }

    /// The panes under @p row, from @p directory.
    [[nodiscard]] std::vector<PaneKey> panesUnder(WorkspaceDirectory const& directory,
                                                  SidebarRowKey const& key)
    {
        auto keys = std::vector<PaneKey> {};
        for (auto const& workspace: directory.workspaces)
        {
            if (workspace.id != key.workspace)
                continue;
            for (auto const& tab: workspace.tabs)
            {
                if (key.kind != SidebarRowKind::Workspace && tab.id != key.tab)
                    continue;
                for (auto const& pane: tab.panes)
                    if (key.kind != SidebarRowKind::Pane || pane.key == key.pane)
                        keys.push_back(pane.key);
            }
        }
        return keys;
    }

    /// The tab of @p directory that holds @p pane.
    [[nodiscard]] std::optional<std::pair<TabRef, std::size_t>> tabHolding(
        WorkspaceDirectory const& directory, PaneKey pane)
    {
        for (auto const& workspace: directory.workspaces)
            for (auto const& tab: workspace.tabs)
                if (std::ranges::contains(tab.panes, pane, &DirectoryPane::key))
                    return std::pair { TabRef { .workspace = workspace.id, .tab = tab.id },
                                       tab.panes.size() };
        return std::nullopt;
    }
} // namespace

QString accessibleNameOf(SidebarRow const& row)
{
    auto name = std::format("{}, {}", row.title, kindWord(row.key.kind));
    switch (row.expansion)
    {
        case RowExpansion::None: break;
        case RowExpansion::Collapsed: name += ", collapsed"; break;
        case RowExpansion::Expanded: name += ", expanded"; break;
    }
    if (row.leaves.needYou > 0)
        name += std::format(", {} needs you", row.leaves.needYou);
    else if (row.leaves.working > 0)
        name += std::format(", {} working", row.leaves.working);
    else
        switch (row.urgency)
        {
            case Urgency::Failed: name += ", failed"; break;
            case Urgency::DoneUnseen: name += ", done"; break;
            case Urgency::UnseenOutput: name += ", new output"; break;
            case Urgency::Idle:
            case Urgency::Working:
            case Urgency::NeedsYou: break;
        }
    if (row.selection == RowSelection::Selected)
        name += ", current";
    return qs(name);
}

SidebarModel::SidebarModel(ISidebarHost& host,
                           TabAttention const& attention,
                           core::platform::IWallClock const& clock,
                           platform::Announcer& announcer,
                           QObject* parent):
    QAbstractListModel(parent),
    _host { host },
    _attention { attention },
    _clock { clock },
    _announcer { announcer }
{
    _timer.setInterval(TickInterval);
    _timer.setTimerType(Qt::CoarseTimer);
    connect(&_timer, &QTimer::timeout, this, &SidebarModel::refreshNow);
    refreshNow();
}

int SidebarModel::rowCount(QModelIndex const& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_rows.size());
}

QHash<int, QByteArray> SidebarModel::roleNames() const
{
    auto names = QHash<int, QByteArray> {};
    for (auto const& entry: RoleNames)
        names.insert(entry.role, entry.name);
    return names;
}

SidebarRow const* SidebarModel::rowAt(int row) const noexcept
{
    if (row < 0 || static_cast<std::size_t>(row) >= _rows.size())
        return nullptr;
    return &_rows[static_cast<std::size_t>(row)];
}

int SidebarModel::rowIndexOf(SidebarRowKey const& key) const noexcept
{
    auto const at = std::ranges::find(_rows, key, &SidebarRow::key);
    return at == _rows.end() ? -1 : static_cast<int>(std::distance(_rows.begin(), at));
}

QVariant SidebarModel::data(QModelIndex const& index, int role) const
{
    auto const* const row = index.isValid() ? rowAt(index.row()) : nullptr;
    if (row == nullptr)
        return {};
    switch (role)
    {
        case KindRole: return static_cast<int>(row->key.kind);
        case DepthRole: return static_cast<int>(row->depth);
        case TitleRole: return qs(row->title);
        case ReasonRole: return qs(row->reason);
        case GitTextRole: return qs(row->gitText);
        case TimeTextRole: return qs(row->time.text);
        case TimeKindRole: return static_cast<int>(row->time.kind);
        case ProgressRole: return row->time.percent ? static_cast<int>(*row->time.percent) : -1;
        case UrgencyRole: return static_cast<int>(row->urgency);
        case GlyphRole: return static_cast<int>(row->glyph);
        case ExpansionRole: return static_cast<int>(row->expansion);
        case SelectedRole: return row->selection == RowSelection::Selected;
        case TreeLinesRole: {
            auto glyphs = QVariantList {};
            for (auto const glyph: row->lines.columns)
                glyphs.push_back(static_cast<int>(glyph));
            return glyphs;
        }
        case AccessibleNameRole: return accessibleNameOf(*row);
        case Qt::DisplayRole: return qs(row->title);
        case Qt::AccessibleTextRole: return accessibleNameOf(*row);
        default: break;
    }
    return {};
}

void SidebarModel::setPresence(SidebarPresence presence)
{
    if (_presence == presence)
        return;
    _presence = presence;
    if (_presence == SidebarPresence::Shown)
        noteChanged(); // The times stood still while nobody looked.
    updateTimer();
}

void SidebarModel::updateTimer()
{
    auto const needed = _presence == SidebarPresence::Shown && std::ranges::any_of(_rows, showsRunningTime);
    if (needed && !_timer.isActive())
        _timer.start();
    else if (!needed && _timer.isActive())
        _timer.stop();
}

void SidebarModel::noteChanged()
{
    if (std::exchange(_rebuildPending, true))
        return;
    QMetaObject::invokeMethod(this, &SidebarModel::refreshNow, Qt::QueuedConnection);
}

WorkspaceDirectory SidebarModel::directory() const
{
    auto const* const source = _host.directorySource();
    return source != nullptr ? source->directory() : WorkspaceDirectory {};
}

SidebarModel::Rebuilt SidebarModel::build(WorkspaceDirectory const& directory) const
{
    auto const now = _clock.now();
    auto const displayed = _host.displayedTab();
    auto built = Rebuilt { .rows = sidebarRows(directory, _attention, _state, now, displayed),
                           .footer = {},
                           .displayed = displayed };
    // The footer counts every pane, whatever the filter hides.
    if (_state.filter.empty())
        built.footer = sidebarFooter(built.rows);
    else
    {
        auto unfiltered = _state;
        unfiltered.filter.clear();
        built.footer = sidebarFooter(sidebarRows(directory, _attention, unfiltered, now, displayed));
    }
    return built;
}

void SidebarModel::refreshNow()
{
    _rebuildPending = false;
    auto const current = directory();
    auto built = build(current);
    announceNewNeeds(current);
    apply(std::move(built.rows));

    if (std::exchange(_footer, built.footer) != built.footer)
        emit footerChanged();
    auto const index = [&] {
        auto const at = std::ranges::find(_rows, RowSelection::Selected, &SidebarRow::selection);
        return at == _rows.end() ? -1 : static_cast<int>(std::distance(_rows.begin(), at));
    }();
    if (std::exchange(_currentIndex, index) != index)
        emit currentIndexChanged();
    _built = true;
    updateTimer();
}

void SidebarModel::apply(std::vector<SidebarRow> next)
{
    if (!_built)
    {
        beginResetModel();
        _rows = std::move(next);
        endResetModel();
        return;
    }

    auto const gone = [&](SidebarRow const& row) {
        return std::ranges::none_of(next, [&](SidebarRow const& other) { return other.key == row.key; });
    };
    // Removals, from the end, so each run's indices stay what they were when it was found.
    for (auto at = static_cast<int>(_rows.size()) - 1; at >= 0;)
    {
        if (!gone(_rows[static_cast<std::size_t>(at)]))
        {
            --at;
            continue;
        }
        auto first = at;
        while (first > 0 && gone(_rows[static_cast<std::size_t>(first - 1)]))
            --first;
        beginRemoveRows({}, first, at);
        _rows.erase(_rows.begin() + first, _rows.begin() + at + 1);
        endRemoveRows();
        at = first - 1;
    }

    // What is left is in the old order: bring it to the new one, a move or an insertion at a time.
    for (auto const [at, row]: core::views::enumerate(next))
    {
        auto const target = static_cast<int>(at);
        if (static_cast<std::size_t>(target) < _rows.size() && _rows[at].key == row.key)
            continue;
        auto const found = std::ranges::find(_rows, row.key, &SidebarRow::key);
        if (found != _rows.end())
        {
            auto const from = static_cast<int>(std::distance(_rows.begin(), found));
            beginMoveRows({}, from, from, {}, target);
            auto moved = std::move(*found);
            _rows.erase(found);
            _rows.insert(_rows.begin() + target, std::move(moved));
            endMoveRows();
        }
        else
        {
            // The run of new rows that follows goes in with it.
            auto last = at;
            while (last + 1 < next.size()
                   && std::ranges::find(_rows, next[last + 1].key, &SidebarRow::key) == _rows.end())
                ++last;
            beginInsertRows({}, target, static_cast<int>(last));
            _rows.insert(_rows.begin() + target,
                         next.begin() + static_cast<std::ptrdiff_t>(at),
                         next.begin() + static_cast<std::ptrdiff_t>(last + 1));
            endInsertRows();
        }
    }

    // Same rows in the same places: tell the view what changed in them, one signal per run of rows.
    auto first = std::optional<int> {};
    auto runRoles = QList<int> {};
    auto const flush = [&](int last) {
        if (!first)
            return;
        emit dataChanged(index(*first), index(last), runRoles);
        first.reset();
        runRoles.clear();
    };
    for (auto const [at, row]: core::views::enumerate(next))
    {
        auto const roles = changedRoles(_rows[at], row);
        if (roles.isEmpty())
        {
            flush(static_cast<int>(at) - 1);
            _rows[at] = row;
            continue;
        }
        if (!first)
            first = static_cast<int>(at);
        for (auto const role: roles)
            if (!runRoles.contains(role))
                runRoles.push_back(role);
        _rows[at] = row;
    }
    flush(static_cast<int>(next.size()) - 1);
}

void SidebarModel::announceNewNeeds(WorkspaceDirectory const& directory)
{
    // The first build, and the first after the window's source was swapped or attached, only learn what
    // already was: a window opening or switching daemons is not news.
    auto const* const source = _host.directorySource();
    auto const baseline = !_built || source != _source;
    _source = source;
    auto urgencies = std::map<PaneKey, Urgency> {};
    for (auto const& workspace: directory.workspaces)
        for (auto const& tab: workspace.tabs)
            for (auto const& pane: tab.panes)
            {
                auto const urgency = paneUrgency(pane, _attention.of(pane.key));
                urgencies.insert_or_assign(pane.key, urgency);
                auto const before = _paneUrgency.find(pane.key);
                auto const rose = urgency == Urgency::NeedsYou
                                  && (before == _paneUrgency.end() || before->second != Urgency::NeedsYou);
                // The first build only learns what already was: a window opening is not news.
                if (baseline || !rose || _activity != WindowActivity::Active
                    || directory.selected == workspace.id)
                    continue;
                auto const& title = !pane.title.empty() ? pane.title : tab.label;
                _announcer.announce(
                    qs(std::format("{} in {} needs you",
                                   title,
                                   workspace.label.empty() ? "a workspace" : workspace.label)),
                    QAccessible::AnnouncementPoliteness::Polite);
            }
    _paneUrgency = std::move(urgencies);
}

void SidebarModel::activate(int row)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr)
        return;
    switch (shown->key.kind)
    {
        case SidebarRowKind::Workspace:
            if (auto* const source = _host.directorySource())
                std::ignore = source->select(shown->key.workspace);
            break;
        case SidebarRowKind::Tab:
        case SidebarRowKind::Pane:
            // A tab shows its first pane; showing one selects its workspace, activates its tab and focuses
            // it, and counts it as seen.
            if (auto const panes = panesUnder(directory(), shown->key); !panes.empty())
                _host.showPane(panes.front());
            break;
    }
    noteChanged();
}

void SidebarModel::setExpansion(int row, RowExpansion wanted)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr || shown->expansion == RowExpansion::None || shown->expansion == wanted)
        return;
    auto const& key = shown->key;
    auto const collapsing = wanted == RowExpansion::Collapsed;
    switch (key.kind)
    {
        case SidebarRowKind::Workspace:
            if (collapsing)
                _state.collapsedWorkspaces.insert(key.workspace);
            else
                _state.collapsedWorkspaces.erase(key.workspace);
            break;
        case SidebarRowKind::Tab: {
            auto const tab = TabRef { .workspace = key.workspace, .tab = key.tab };
            if (collapsing)
                _state.collapsedTabs.insert(tab);
            else
                _state.collapsedTabs.erase(tab);
            break;
        }
        case SidebarRowKind::Pane: return;
    }
    refreshNow();
}

void SidebarModel::toggle(int row)
{
    if (auto const* const shown = rowAt(row))
        setExpansion(row,
                     shown->expansion == RowExpansion::Expanded ? RowExpansion::Collapsed
                                                                : RowExpansion::Expanded);
}

void SidebarModel::expand(int row)
{
    setExpansion(row, RowExpansion::Expanded);
}

void SidebarModel::collapse(int row)
{
    setExpansion(row, RowExpansion::Collapsed);
}

void SidebarModel::rename(int row, QString const& name)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr || shown->key.kind != SidebarRowKind::Workspace)
        return;
    _host.renameWorkspace(shown->key.workspace, name.toStdString());
}

void SidebarModel::markSeen(int row)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr)
        return;
    for (auto const pane: panesUnder(directory(), shown->key))
        _host.markSeen(pane);
    noteChanged();
}

void SidebarModel::openContextMenu(int row, QPointF windowPosition)
{
    if (auto const* const shown = rowAt(row))
        _host.openContextMenu(*shown, windowPosition);
}

void SidebarModel::setFilter(QString const& text)
{
    auto filter = text.toStdString();
    if (filter == _state.filter)
        return;
    _state.filter = std::move(filter);
    refreshNow();
}

void SidebarModel::drop(int row, double y, QString const& payload)
{
    auto const drag = parseNodeDrag(payload);
    if (!drag)
        return;
    auto const current = directory();
    auto const paneKey = [&]() -> std::optional<PaneKey> {
        if (drag->sessions.empty())
            return std::nullopt;
        return PaneKey { drag->sessions.front() };
    }();
    auto subject = DragSubject::Pane;
    switch (drag->payload)
    {
        case DropPayload::Workspace: subject = DragSubject::Workspace; break;
        case DropPayload::Tab: subject = DragSubject::Tab; break;
        case DropPayload::Pane:
            // A pane that is its tab's only one is the tab, as the tab strip takes it.
            if (paneKey)
                if (auto const held = tabHolding(current, *paneKey); held && held->second == 1)
                    subject = DragSubject::Tab;
            break;
        case DropPayload::None:
        case DropPayload::Files:
        case DropPayload::Text: return;
    }

    auto const outcome = resolveSidebarDrop(_rows, row, y, subject);
    if (auto const* const reorder = std::get_if<ReorderWorkspace>(&outcome))
        _host.moveWorkspace(drag->workspace, reorder->to);
    else if (auto const* const move = std::get_if<MoveTab>(&outcome); move != nullptr && paneKey)
        if (auto const held = tabHolding(current, *paneKey))
            _host.moveTab(held->first, move->toWorkspace, move->toPosition);
}

} // namespace contour::window
