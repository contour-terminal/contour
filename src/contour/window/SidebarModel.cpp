// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ColorConversion.hpp>
#include <contour/window/AttentionJump.hpp>
#include <contour/window/ChromeKeys.hpp>
#include <contour/window/DaemonLinkLook.hpp>
#include <contour/window/DisclosureWords.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/QtString.hpp>
#include <contour/window/SidebarDaemonRow.hpp>
#include <contour/window/SidebarDropPolicy.hpp>
#include <contour/window/SidebarModel.hpp>
#include <contour/window/SidebarSections.hpp>
#include <contour/window/TabIconVariant.hpp>

#include <core/Overloaded.hpp>
#include <core/Utils.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>
#include <QtGui/QColor>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

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
        RoleName { SidebarModel::ProgramRole, "program" },
        RoleName { SidebarModel::FolderRole, "folder" },
        RoleName { SidebarModel::ReasonLabelRole, "reasonLabel" },
        RoleName { SidebarModel::ReasonCommandRole, "reasonCommand" },
        RoleName { SidebarModel::TreeTextRole, "treeText" },
        RoleName { SidebarModel::MeterTextRole, "meterText" },
        RoleName { SidebarModel::HoverDetailRole, "hoverDetail" },
        RoleName { SidebarModel::ProgressFractionRole, "progressFraction" },
        RoleName { SidebarModel::FullReasonRole, "fullReason" },
        RoleName { SidebarModel::FullReasonLabelRole, "fullReasonLabel" },
        RoleName { SidebarModel::FullReasonCommandRole, "fullReasonCommand" },
        RoleName { SidebarModel::AccessibleDescriptionRole, "accessibleDescription" },
        RoleName { SidebarModel::SelectionRole, "selection" },
        RoleName { SidebarModel::TreeTextContinuedRole, "treeTextContinued" },
        RoleName { SidebarModel::SecondLineRole, "secondLine" },
        RoleName { SidebarModel::IdleLineRole, "idleLine" },
        RoleName { SidebarModel::TabColorRole, "tabColor" },
        RoleName { SidebarModel::WorkspaceColorRole, "workspaceColor" },
        RoleName { SidebarModel::RailTextRole, "railText" },
        RoleName { SidebarModel::RailTextContinuedRole, "railTextContinued" },
        RoleName { SidebarModel::IconRole, "icon" },
        RoleName { SidebarModel::LinkStateRole, "linkState" },
        RoleName { SidebarModel::LinkReasonRole, "linkReason" },
        RoleName { SidebarModel::LocalityRole, "locality" },
        RoleName { SidebarModel::StalenessRole, "staleness" },
        RoleName { SidebarModel::ShownElsewhereRole, "shownElsewhere" },
        RoleName { SidebarModel::PendingRole, "pending" },
        RoleName { SidebarModel::StatusSlotRole, "statusSlot" },
        RoleName { SidebarModel::DaemonNameRole, "daemonName" },
        RoleName { SidebarModel::DaemonButtonsRole, "daemonButtons" },
    };

    [[nodiscard]] QString qs(std::string const& text)
    {
        return QString::fromStdString(text);
    }

    /// @return What a row of @p kind is, as a screen reader says it.
    [[nodiscard]] QString kindWord(SidebarRowKind kind)
    {
        switch (kind)
        {
            case SidebarRowKind::Workspace: return SidebarModel::tr("Workspace");
            case SidebarRowKind::Tab: return SidebarModel::tr("Tab");
            case SidebarRowKind::Pane: return SidebarModel::tr("Pane");
            case SidebarRowKind::Daemon: return SidebarModel::tr("Daemon");
            case SidebarRowKind::Hidden: return SidebarModel::tr("Hidden daemons");
        }
        return {};
    }

    /// One count of panes, as words in the plural form @p count takes. The English source text carries both
    /// forms, so it reads right where no translation is loaded; a translation picks its own forms by %n.
    struct CountWords
    {
        char const* one;   ///< The source text for one pane.
        char const* other; ///< The source text for any other count.
    };

    /// @return @p words for @p count, translated.
    [[nodiscard]] QString counted(CountWords const& words, uint32_t count)
    {
        auto const n = static_cast<int>(count);
        return SidebarModel::tr(count == 1 ? words.one : words.other, nullptr, n);
    }

    constexpr auto PanesNeedYou =
        CountWords { .one = QT_TR_NOOP("%n pane needs you"), .other = QT_TR_NOOP("%n panes need you") };
    constexpr auto PanesWorking =
        CountWords { .one = QT_TR_NOOP("%n pane working"), .other = QT_TR_NOOP("%n panes working") };
    constexpr auto HiddenDaemons =
        CountWords { .one = QT_TR_NOOP("%n hidden daemon"), .other = QT_TR_NOOP("%n hidden daemons") };
    constexpr auto FooterNeedYou =
        CountWords { .one = QT_TR_NOOP("%n needs you"), .other = QT_TR_NOOP("%n need you") };
    constexpr auto FooterWorking =
        CountWords { .one = QT_TR_NOOP("%n working"), .other = QT_TR_NOOP("%n working") };
    constexpr auto FooterNeedYouCompact =
        CountWords { .one = QT_TR_NOOP("%n waiting"), .other = QT_TR_NOOP("%n waiting") };
    constexpr auto FooterWorkingCompact =
        CountWords { .one = QT_TR_NOOP("%n busy"), .other = QT_TR_NOOP("%n busy") };

    /// @return What @p urgency says of a row, translated; empty for Idle.
    [[nodiscard]] QString urgencyWords(Urgency urgency)
    {
        switch (urgency)
        {
            case Urgency::Idle: return {};
            case Urgency::UnseenOutput: return SidebarModel::tr("new output");
            case Urgency::Working: return SidebarModel::tr("working");
            case Urgency::DoneUnseen: return SidebarModel::tr("done");
            case Urgency::Failed: return SidebarModel::tr("failed");
            case Urgency::NeedsYou: return SidebarModel::tr("needs you");
        }
        return {};
    }

    /// @return What @p state says of a daemon's link, as a screen reader says it, translated.
    [[nodiscard]] QString linkWords(DaemonLinkState state)
    {
        return QCoreApplication::translate(
            std::string(DaemonLinkContext).c_str(),
            std::string(DaemonLinkLooks[static_cast<std::size_t>(state)].accessibleWords).c_str());
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
        note(before.reason == after.reason && before.reasonLabel == after.reasonLabel
                 && before.reasonSeparator == after.reasonSeparator
                 && before.reasonCommand == after.reasonCommand,
             { SidebarModel::ReasonRole, SidebarModel::ReasonLabelRole, SidebarModel::ReasonCommandRole });
        note(before.fullReason == after.fullReason && before.fullReasonLabel == after.fullReasonLabel
                 && before.fullReasonCommand == after.fullReasonCommand,
             { SidebarModel::FullReasonRole,
               SidebarModel::FullReasonLabelRole,
               SidebarModel::FullReasonCommandRole });
        note(before.program == after.program, { SidebarModel::ProgramRole });
        note(before.folder == after.folder, { SidebarModel::FolderRole });
        note(hoverDetailOf(before) == hoverDetailOf(after), { SidebarModel::HoverDetailRole });
        note(before.gitText == after.gitText, { SidebarModel::GitTextRole });
        note(before.time.text == after.time.text, { SidebarModel::TimeTextRole });
        note(before.time.kind == after.time.kind, { SidebarModel::TimeKindRole });
        note(before.time.percent == after.time.percent,
             { SidebarModel::ProgressRole, SidebarModel::MeterTextRole, SidebarModel::ProgressFractionRole });
        note(before.urgency == after.urgency, { SidebarModel::UrgencyRole });
        note(before.glyph == after.glyph, { SidebarModel::GlyphRole });
        note(before.icon == after.icon, { SidebarModel::IconRole });
        note(before.expansion == after.expansion, { SidebarModel::ExpansionRole });
        note(before.selection == after.selection, { SidebarModel::SelectedRole });
        note(before.lines == after.lines && before.depth == after.depth,
             { SidebarModel::TreeLinesRole, SidebarModel::TreeTextRole });
        note(before.selection == after.selection, { SidebarModel::SelectionRole });
        note(before.continued == after.continued && before.depth == after.depth,
             { SidebarModel::TreeTextContinuedRole });
        note(secondLineOf(before) == secondLineOf(after), { SidebarModel::SecondLineRole });
        note(idleLineOf(before) == idleLineOf(after), { SidebarModel::IdleLineRole });
        note(before.tabColor == after.tabColor, { SidebarModel::TabColorRole });
        note(before.workspaceColor == after.workspaceColor, { SidebarModel::WorkspaceColorRole });
        note(workspaceRailCellsOf(before) == workspaceRailCellsOf(after),
             { SidebarModel::RailTextRole, SidebarModel::RailTextContinuedRole });
        note(before.linkState == after.linkState, { SidebarModel::LinkStateRole });
        note(before.linkReason == after.linkReason, { SidebarModel::LinkReasonRole });
        note(before.locality == after.locality, { SidebarModel::LocalityRole });
        note(before.staleness == after.staleness, { SidebarModel::StalenessRole });
        note(before.shownElsewhere == after.shownElsewhere, { SidebarModel::ShownElsewhereRole });
        note(before.pending == after.pending, { SidebarModel::PendingRole });
        note(before.slot == after.slot, { SidebarModel::StatusSlotRole });
        note(before.daemonName == after.daemonName, { SidebarModel::DaemonNameRole });
        note(daemonRowFactsOf(before) == daemonRowFactsOf(after) && before.pending == after.pending,
             { SidebarModel::DaemonButtonsRole });
        // The spoken name is the title; the description folds in the kind and the status, not a running time.
        if (accessibleNameOf(before) != accessibleNameOf(after))
            for (auto const role: { static_cast<int>(SidebarModel::AccessibleNameRole),
                                    static_cast<int>(Qt::AccessibleTextRole) })
                if (!roles.contains(role))
                    roles.push_back(role);
        if (accessibleDescriptionOf(before) != accessibleDescriptionOf(after))
            roles.push_back(SidebarModel::AccessibleDescriptionRole);
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
        if (key.kind == SidebarRowKind::Daemon || key.kind == SidebarRowKind::Hidden)
            return keys; // names no workspace
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

    /// @return The jump mode @p mode names, as QML passes it; Here for anything else.
    [[nodiscard]] JumpMode jumpModeOf(int mode) noexcept
    {
        return mode == static_cast<int>(std::to_underlying(JumpMode::NewWindow)) ? JumpMode::NewWindow
                                                                                 : JumpMode::Here;
    }

    /// @param disclosure Whether a row's children show.
    /// @return The word a screen reader says for it, translated once for every control that says it.
    QString disclosureText(Disclosure disclosure)
    {
        return QCoreApplication::translate(std::string(DisclosureContext).c_str(),
                                           std::string(disclosureWord(disclosure)).c_str());
    }

} // namespace

void registerSidebarQmlTypes()
{
    // Function-local, so the first call registers and every later one returns: the application registers
    // once at start, the test runner once after its application object is built.
    static auto const registered = [] {
        qmlRegisterUncreatableMetaObject(sidebarrows::staticMetaObject,
                                         "Contour.Terminal",
                                         1,
                                         0,
                                         "SidebarRows",
                                         QStringLiteral("SidebarRows holds enums only."));
        return true;
    }();
    (void) registered;
}

QString accessibleNameOf(SidebarRow const& row)
{
    // The Hidden row shows no title of its own; it is named for what it is.
    if (row.key.kind == SidebarRowKind::Hidden && row.title.empty())
        return kindWord(row.key.kind);
    return qs(row.title);
}

QString accessibleDescriptionOf(SidebarRow const& row)
{
    if (row.key.kind == SidebarRowKind::Hidden)
        return counted(HiddenDaemons, static_cast<uint32_t>(row.hidden.size()));
    auto const daemon = row.key.kind == SidebarRowKind::Daemon;
    auto parts = QStringList { kindWord(row.key.kind) };
    // A daemon row says its link first: the words of its state, never its dot or its mark.
    if (daemon)
        parts.push_back(linkWords(row.linkState));
    // The fold is a state a tree item would carry; Qt Quick's Accessible has none for it, so it is said here.
    switch (row.expansion)
    {
        case RowExpansion::None: break;
        case RowExpansion::Collapsed: parts.push_back(disclosureText(Disclosure::Collapsed)); break;
        case RowExpansion::Expanded: parts.push_back(disclosureText(Disclosure::Expanded)); break;
    }
    if (daemon && !row.linkReason.empty())
        parts.push_back(qs(row.linkReason));
    if (row.staleness == RowStaleness::LastKnown)
        parts.push_back(SidebarModel::tr("last known"));
    if (row.shownElsewhere == ShownElsewhere::Yes)
        parts.push_back(SidebarModel::tr("shown in another window"));
    // A daemon row already connecting says so in its link words.
    if (row.pending == RowPending::Yes && !(daemon && row.linkState == DaemonLinkState::Connecting))
        parts.push_back(SidebarModel::tr("connecting"));
    if (row.leaves.needYou > 0)
        parts.push_back(counted(PanesNeedYou, row.leaves.needYou));
    else if (row.leaves.working > 0)
        parts.push_back(counted(PanesWorking, row.leaves.working));
    // A row's own working or needing pane is counted above; what else it says, it says in words. A daemon
    // row's words are its link's: its panes' are only counted.
    else if (!daemon && row.urgency != Urgency::Working && row.urgency != Urgency::NeedsYou)
        if (auto words = urgencyWords(row.urgency); !words.isEmpty())
            parts.push_back(std::move(words));
    if (row.selection == RowSelection::ContainsSelected)
        parts.push_back(SidebarModel::tr("holds the current tab"));
    return parts.join(QStringLiteral(", "));
}

QString footerTextOf(SidebarFooter const& footer)
{
    auto parts = QStringList {};
    if (footer.needYou > 0)
        parts.push_back(counted(FooterNeedYou, footer.needYou));
    if (footer.working > 0)
        parts.push_back(counted(FooterWorking, footer.working));
    return parts.join(QStringLiteral(" · "));
}

QString footerCompactTextOf(SidebarFooter const& footer)
{
    auto parts = QStringList {};
    if (footer.needYou > 0)
        parts.push_back(counted(FooterNeedYouCompact, footer.needYou));
    if (footer.working > 0)
        parts.push_back(counted(FooterWorkingCompact, footer.working));
    return parts.join(QStringLiteral(" · "));
}

SidebarModel::SidebarModel(ISidebarHost& host,
                           IDaemonSections& sections,
                           TabAttention const& attention,
                           core::platform::IWallClock const& clock,
                           platform::Announcer& announcer,
                           QObject* parent):
    QAbstractListModel(parent),
    _host { host },
    _sections { sections },
    _attention { attention },
    _clock { clock },
    _announcer { announcer },
    _sectionsChanged { sections.onChanged([this] { noteChanged(); }) }
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

SidebarRow const* SidebarModel::daemonRowAt(int row) const noexcept
{
    auto const* const shown = rowAt(row);
    return shown != nullptr && shown->key.kind == SidebarRowKind::Daemon ? shown : nullptr;
}

bool SidebarModel::isOwn(SidebarRowKey const& key) const noexcept
{
    return !_own || key.daemon == _own->daemon;
}

SidebarRow const* SidebarModel::ownRowAt(int row) const noexcept
{
    auto const* const shown = rowAt(row);
    return shown != nullptr && isOwn(shown->key) ? shown : nullptr;
}

bool SidebarModel::isOwnRow(int row) const noexcept
{
    return ownRowAt(row) != nullptr;
}

SidebarRowKey SidebarModel::ownWorkspaceKey(uint64_t workspace) const
{
    return SidebarRowKey { .kind = SidebarRowKind::Workspace,
                           .daemon = _own ? _own->daemon : DaemonRef {},
                           .instance = _own ? _own->instance : DaemonInstance {},
                           .workspace = workspace,
                           .tab = 0,
                           .pane = {} };
}

QVariantMap sidebarCommandMap()
{
    auto commands = QVariantMap {};
    for (auto const& [name, command]: SidebarCommandNames)
        commands.insert(QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())),
                        static_cast<int>(command));
    return commands;
}

int SidebarModel::collapseOrParent(int row)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr)
        return row;
    if (shown->expansion == RowExpansion::Expanded)
    {
        collapse(row);
        return row;
    }
    auto const parent = parentRowOf(_rows, row);
    return parent >= 0 ? parent : row;
}

int SidebarModel::expandOrChild(int row)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr)
        return row;
    switch (shown->expansion)
    {
        case RowExpansion::None: return row;
        case RowExpansion::Collapsed: expand(row); return row;
        case RowExpansion::Expanded: return parentRowOf(_rows, row + 1) == row ? row + 1 : row;
    }
    return row;
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
        case FullReasonRole: return qs(row->fullReason);
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
        case ProgramRole: return qs(row->program);
        case FolderRole: return qs(row->folder);
        // The sidebar always draws the command (elided), so the separator is drawn with the words.
        case ReasonLabelRole: return qs(row->reasonLabel + row->reasonSeparator);
        case ReasonCommandRole: return qs(row->reasonCommand);
        case FullReasonLabelRole: return qs(row->fullReasonLabel);
        case FullReasonCommandRole: return qs(row->fullReasonCommand);
        case TreeTextRole: return qs(treeLinesText(row->lines, row->depth));
        case MeterTextRole: return row->time.percent ? qs(meterText(*row->time.percent)) : QString {};
        case HoverDetailRole: return qs(hoverDetailOf(*row));
        case ProgressFractionRole:
            return row->time.percent ? std::clamp(static_cast<double>(*row->time.percent), 0.0, 100.0) / 100.0
                                     : 0.0;
        case Qt::DisplayRole: return qs(row->title);
        case Qt::AccessibleTextRole: return accessibleNameOf(*row);
        case Qt::AccessibleDescriptionRole:
        case AccessibleDescriptionRole: return accessibleDescriptionOf(*row);
        case SelectionRole: return static_cast<int>(row->selection);
        case TreeTextContinuedRole: return qs(treeContinuationText(*row));
        case SecondLineRole: return static_cast<int>(secondLineOf(*row));
        case IdleLineRole: return qs(idleLineOf(*row));
        case TabColorRole:
            return row->tabColor ? platform::toQColor(*row->tabColor) : QColor { Qt::transparent };
        case WorkspaceColorRole:
            return row->workspaceColor ? platform::toQColor(*row->workspaceColor)
                                       : QColor { Qt::transparent };
        case RailTextRole: return qs(workspaceRailCellsOf(*row).first);
        case RailTextContinuedRole: return qs(workspaceRailCellsOf(*row).second);
        case IconRole: return row->icon ? tabIconVariant(*row->icon) : QVariantMap {};
        case LinkStateRole: return static_cast<int>(row->linkState);
        case LinkReasonRole: return qs(row->linkReason);
        case LocalityRole: return static_cast<int>(row->locality);
        case StalenessRole: return static_cast<int>(row->staleness);
        case ShownElsewhereRole: return static_cast<int>(row->shownElsewhere);
        case PendingRole: return static_cast<int>(row->pending);
        case StatusSlotRole: return static_cast<int>(row->slot);
        case DaemonNameRole: return qs(row->daemonName);
        case DaemonButtonsRole: {
            auto buttons = QVariantList {};
            if (row->key.kind == SidebarRowKind::Daemon)
                for (auto const button: daemonRowButtons(daemonRowFactsOf(*row), row->pending))
                    buttons.push_back(static_cast<int>(button));
            return buttons;
        }
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

void SidebarModel::followSections(SectionsFeed feed)
{
    if (feed == sectionsFeed())
        return;
    switch (feed)
    {
        case SectionsFeed::Dropped:
            _sectionsChanged = SectionsSubscription {};
            _timer.stop();
            if (_rowsState == RowsState::Stale)
                _rowsState = RowsState::Current;
            break;
        case SectionsFeed::Followed:
            _sectionsChanged = _sections.onChanged([this] { noteChanged(); });
            noteChanged();
            break;
    }
}

void SidebarModel::noteChanged()
{
    if (std::exchange(_rowsState, RowsState::Stale) == RowsState::Stale)
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
    auto const displayed = _host.displayedTab(directory);
    // A window that follows no selection shows the workspace of the tab it displays.
    auto current = directory.selected;
    if (!current && displayed)
        current = displayed->workspace;

    // A window on a daemon lists every daemon listed, its own section fed by its own link; a window with no
    // daemon lists its own workspaces alone.
    auto sections = std::vector<DaemonSection> {};
    auto listing = SidebarListing {};
    if (_own)
    {
        auto listed = _sections.sections();
        auto own = DaemonSection { .daemon = _own->daemon,
                                   .instance = _own->instance,
                                   .name = _own->name,
                                   .locality = _own->locality,
                                   .state = _own->state,
                                   .reason = {},
                                   .directory = directory,
                                   .shownElsewhere = {},
                                   .feed = SectionFeed::WindowLink };
        if (auto const at = std::ranges::find(listed, own.daemon, &DaemonSection::daemon); at != listed.end())
            own.shownElsewhere = at->shownElsewhere;
        sections = windowSections(std::move(listed), std::move(own), current);
        auto const shown = displayed ? std::optional { SidebarTarget { .daemon = _own->daemon,
                                                                       .instance = _own->instance,
                                                                       .workspace = displayed->workspace,
                                                                       .tab = displayed->tab,
                                                                       .pane = {} } }
                                     : std::nullopt;
        listing = sidebarListing(
            SidebarSources {
                .sections = sections, .own = _own->daemon, .ownMarks = &_attention, .hidden = {} },
            _state,
            _clock.now(),
            shown,
            _host.pendingJump());
    }
    else
        listing = sidebarListing(directory, _attention, _state, _clock.now(), displayed);

    // A tab's icon is the window's to say: the directory knows neither its profile nor what runs in it.
    for (auto& row: listing.rows)
        if (row.key.kind == SidebarRowKind::Tab && isOwn(row.key))
            if (auto const listed = findTab(directory, row.key.workspace, row.key.tab))
                row.icon = _host.tabIcon(*listed->tab);
    // The footer and the summary count every pane and workspace, whatever the filter hides.
    auto footer = sidebarFooter(listing.workspaces);
    auto summary = sidebarSummary(
        listing.workspaces,
        current ? std::optional { WorkspaceKey { .daemon = _own ? _own->daemon : DaemonRef {},
                                                 .instance = _own ? _own->instance : DaemonInstance {},
                                                 .workspace = *current } }
                : std::nullopt);
    // The switcher lists the window's own daemon's workspaces from here; the other daemons it lists itself.
    std::erase_if(listing.workspaces, [this](SidebarRow const& row) { return !isOwn(row.key); });
    return Rebuilt { .rows = std::move(listing.rows),
                     .workspaces = std::move(listing.workspaces),
                     .footer = footer,
                     .summary = std::move(summary),
                     .displayed = displayed,
                     .currentWorkspace = current,
                     .sections = std::move(sections) };
}

void SidebarModel::refreshNow()
{
    // A dropped sidebar's window is closing: a rebuild queued before the drop reads nothing more.
    if (sectionsFeed() == SectionsFeed::Dropped)
    {
        _rowsState = RowsState::Current;
        return;
    }
    auto const unbuilt = std::exchange(_rowsState, RowsState::Current) == RowsState::Unbuilt;
    _own = _host.ownDaemon();
    // The daemon the window opened on keeps its workspaces unfolded; every other daemon's start folded.
    if (_own && _state.openedOn.value.empty())
        _state.openedOn = _own->daemon;
    auto const current = directory();
    auto built = build(current);
    announceNewNeeds(built.sections, current);
    if (unbuilt)
        resetRows(std::move(built.rows));
    else
        apply(std::move(built.rows));

    auto const workspacesMoved = _workspaceRows != built.workspaces;
    _workspaceRows = std::move(built.workspaces);
    if (std::exchange(_footer, built.footer) != built.footer)
        emit footerChanged();
    auto const index = [&] {
        // The displayed tab's row, or the folded workspace that holds it.
        auto const at = std::ranges::find_if(
            _rows, [](SidebarRow const& row) { return row.selection != RowSelection::None; });
        return at == _rows.end() ? -1 : static_cast<int>(std::distance(_rows.begin(), at));
    }();
    auto const key = index >= 0 ? std::optional { _rows[static_cast<std::size_t>(index)].key } : std::nullopt;
    auto const indexMoved = std::exchange(_currentIndex, index) != index;
    auto const rowMoved = std::exchange(_currentKey, key) != key;
    if (rowMoved || indexMoved)
        emit currentIndexChanged();
    if (rowMoved && key)
        emit currentRowChanged();
    auto const workspaceIndex =
        built.currentWorkspace ? rowIndexOf(ownWorkspaceKey(*built.currentWorkspace)) : -1;
    auto const daemonTitle = _own ? _own->name : std::string {};
    auto const countMoved = std::exchange(_sectionCount, built.sections.size()) != built.sections.size();
    auto const titleMoved = std::exchange(_daemonTitle, daemonTitle) != daemonTitle;
    auto const sectionsMoved = countMoved || titleMoved;
    auto const summaryMoved = std::exchange(_summary, built.summary) != built.summary || sectionsMoved;
    if (std::exchange(_currentWorkspaceIndex, workspaceIndex) != workspaceIndex || summaryMoved)
        emit summaryChanged();
    if (workspacesMoved)
        emit workspaceRowsChanged();
    auto const empty = sidebarEmptyStateOf(_state.scope, rowsNeedingMe(_rows));
    if (std::exchange(_emptyState, empty) != empty)
        emit emptyStateChanged();
    updateTimer();
}

void SidebarModel::resetRows(std::vector<SidebarRow> next)
{
    beginResetModel();
    _rows = std::move(next);
    endResetModel();
}

void SidebarModel::apply(std::vector<SidebarRow> next)
{

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

void SidebarModel::announceNewNeeds(std::span<DaemonSection const> sections, WorkspaceDirectory const& own)
{
    // A swapped or attached source is a new baseline: a window switching daemons is not news. No source was
    // read before the first build, so it is one whose source differs; and one with no source lists nothing.
    auto const* const source = _host.directorySource();
    auto const swapped = source != _source;
    _source = source;
    auto urgencies = std::map<SidebarRowKey, Urgency> {};
    auto runs = std::set<SectionRun> {};
    auto const visit = [&](DaemonRef const& daemon,
                           DaemonInstance instance,
                           std::string const& name,
                           WorkspaceDirectory const& directory) {
        // A section first listed, or listed under a new run of its daemon, only learns what already was: a
        // daemon listed, connected or restarted is not news.
        auto run = SectionRun { daemon, instance };
        auto const baseline = swapped || !_sectionRuns.contains(run);
        runs.insert(std::move(run));
        // The window's own panes are read against its own seen-state; another daemon's as it states them.
        auto const ownSection = isOwn(SidebarRowKey { .kind = SidebarRowKind::Pane,
                                                      .daemon = daemon,
                                                      .instance = instance,
                                                      .workspace = 0,
                                                      .tab = 0,
                                                      .pane = {} });
        for (auto const& workspace: directory.workspaces)
            for (auto const& tab: workspace.tabs)
                for (auto const& pane: tab.panes)
                {
                    auto const key = SidebarRowKey { .kind = SidebarRowKind::Pane,
                                                     .daemon = daemon,
                                                     .instance = instance,
                                                     .workspace = workspace.id,
                                                     .tab = tab.id,
                                                     .pane = pane.key };
                    auto const urgency =
                        paneUrgency(pane, ownSection ? _attention.of(pane.key) : SessionAttention {});
                    urgencies.insert_or_assign(key, urgency);
                    auto const before = _paneUrgency.find(key);
                    auto const rose =
                        urgency == Urgency::NeedsYou
                        && (before == _paneUrgency.end() || before->second != Urgency::NeedsYou);
                    // A baseline, a window without the user's attention and the workspace it shows say
                    // nothing.
                    if (baseline || !rose || _activity != WindowActivity::Active
                        || (ownSection && directory.selected == workspace.id))
                        continue;
                    auto const title = qs(!pane.title.empty() ? pane.title : tab.name);
                    auto const where = workspace.name.empty() ? tr("a workspace") : qs(workspace.name);
                    // Another daemon's need says whose it is.
                    _announcer.announce(ownSection
                                            ? tr("%1 in %2 needs you").arg(title, where)
                                            : tr("%1 in %2 on %3 needs you").arg(title, where, qs(name)),
                                        QAccessible::AnnouncementPoliteness::Polite);
                }
    };
    if (sections.empty())
        visit(DaemonRef {}, DaemonInstance {}, std::string {}, own);
    for (auto const& section: sections)
        visit(section.daemon, section.instance, section.name, section.directory);
    _paneUrgency = std::move(urgencies);
    _sectionRuns = std::move(runs);
}

SidebarModel::JumpPlace SidebarModel::placeOf(SidebarTarget const& target) const
{
    auto place = JumpPlace {};
    auto const named = [&](std::string const& name, WorkspaceDirectory const& directory) {
        place.daemon = qs(name);
        if (auto const workspace =
                std::ranges::find(directory.workspaces, target.workspace, &DirectoryWorkspace::id);
            workspace != directory.workspaces.end())
            place.workspace = qs(workspace->name);
    };
    if (!_own || target.daemon == _own->daemon)
        named(_own ? _own->name : std::string {}, directory());
    else
    {
        auto const listed = _sections.sections();
        if (auto const section = std::ranges::find(listed, target.daemon, &DaemonSection::daemon);
            section != listed.end())
            named(section->name, section->directory);
    }
    // A daemon the window has no name for is called what its reference calls it.
    if (place.daemon.isEmpty())
    {
        auto const parsed = parseDaemonRef(target.daemon);
        place.daemon = parsed ? qs(std::string { parsed->second }) : qs(target.daemon.value);
    }
    return place;
}

void SidebarModel::noteJump(JumpEvent event, SidebarTarget const& target, std::string const& reason)
{
    auto const place = placeOf(target);
    auto const message = [&] {
        switch (event)
        {
            case JumpEvent::Connecting: return tr("Connecting to %1…").arg(place.daemon);
            case JumpEvent::Landed:
                return place.workspace.isEmpty() ? tr("Now on %1").arg(place.daemon)
                                                 : tr("Now on %1 › %2").arg(place.daemon, place.workspace);
            case JumpEvent::Cancelled: return tr("Jump cancelled");
            case JumpEvent::Failed:
                return reason.empty() ? tr("Could not reach %1").arg(place.daemon)
                                      : tr("Could not reach %1: %2").arg(place.daemon, qs(reason));
        }
        return QString {};
    }();
    _announcer.announce(message, QAccessible::AnnouncementPoliteness::Polite);
}

void SidebarModel::activate(int row)
{
    std::ignore = activateRow(row);
}

sidebarrows::RowActivation SidebarModel::activateRow(int row)
{
    return activateRow(row, JumpMode::Here);
}

sidebarrows::RowActivation SidebarModel::activateRow(int row, JumpMode mode)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr)
        return sidebarrows::RowActivation::Nothing;
    switch (shown->key.kind)
    {
        case SidebarRowKind::Daemon: {
            auto const tree =
                shown->expansion == RowExpansion::None ? LastKnownTree::Absent : LastKnownTree::Present;
            switch (daemonRowActivation(daemonRowFactsOf(*shown), tree))
            {
                case DaemonRowAction::Nothing: break;
                case DaemonRowAction::ToggleFold: toggle(row); break;
                case DaemonRowAction::Connect: connectDaemon(row); break;
                case DaemonRowAction::Start: startDaemon(row); break;
            }
            // The keyboard stays on the daemon row, whatever it did.
            return sidebarrows::RowActivation::Nothing;
        }
        case SidebarRowKind::Hidden: return sidebarrows::RowActivation::Nothing;
        case SidebarRowKind::Workspace:
        case SidebarRowKind::Tab:
        case SidebarRowKind::Pane: break;
    }
    // Another daemon's row, and any row opened beside, is jumped to; the keyboard stays where it was.
    if (mode == JumpMode::NewWindow || !isOwn(shown->key))
    {
        _host.jumpTo(sidebarTargetOf(shown->key), mode);
        return sidebarrows::RowActivation::Nothing;
    }
    switch (shown->key.kind)
    {
        case SidebarRowKind::Workspace: _host.showWorkspace(shown->key.workspace); break;
        case SidebarRowKind::Tab:
        case SidebarRowKind::Pane:
            // A tab shows its first pane; showing one selects its workspace, activates its tab and focuses
            // it, and counts it as seen.
            if (auto const panes = panesUnder(directory(), shown->key); !panes.empty())
                _host.showPane(panes.front());
            break;
        case SidebarRowKind::Daemon:
        case SidebarRowKind::Hidden: break;
    }
    noteChanged();
    return sidebarrows::RowActivation::Shown;
}

sidebarrows::RowActivation SidebarModel::activateFirstMatch(int mode)
{
    if (isUnnarrowed(_state))
        return sidebarrows::RowActivation::Unfiltered;
    auto const first = firstMatchOf(_rows, _state);
    if (!first)
        return sidebarrows::RowActivation::Nothing;
    return activateRow(static_cast<int>(*first), jumpModeOf(mode));
}

bool SidebarModel::isJumpPending() const
{
    return _host.pendingJump().has_value();
}

void SidebarModel::cancelJump()
{
    _host.cancelJump();
}

void SidebarModel::connectDaemon(int row)
{
    auto const* const shown = daemonRowAt(row);
    if (shown == nullptr)
        return;
    switch (connectRouteOf(daemonRowFactsOf(*shown)))
    {
        case ConnectRoute::None: break;
        case ConnectRoute::Sections: _sections.connect(shown->key.daemon, _host.windowId()); break;
        case ConnectRoute::Reconnect: _host.reconnect(); break;
        case ConnectRoute::Prompt: _host.raisePrompt(); break;
    }
}

void SidebarModel::startDaemon(int row)
{
    if (auto const* const shown = daemonRowAt(row))
        _sections.start(shown->key.daemon);
}

void SidebarModel::removeDaemon(int row)
{
    if (auto const* const shown = daemonRowAt(row))
        _sections.remove(shown->key.daemon);
}

void SidebarModel::cancelPending(int row)
{
    auto const* const shown = daemonRowAt(row);
    if (shown == nullptr)
        return;
    switch (cancelRouteOf(shown->key.daemon, _host.pendingJump(), shown->linkState))
    {
        case CancelRoute::None: break;
        case CancelRoute::Jump: _host.cancelJump(); break;
        case CancelRoute::Connect: _sections.disconnect(shown->key.daemon); break;
    }
}

void SidebarModel::pressDaemonButton(int row, int button)
{
    if (button < 0 || std::cmp_greater_equal(button, DaemonRowButtonCount))
        return;
    switch (static_cast<DaemonRowButton>(button))
    {
        case DaemonRowButton::None: break;
        case DaemonRowButton::Connect: connectDaemon(row); break;
        case DaemonRowButton::Start: startDaemon(row); break;
        case DaemonRowButton::Cancel: cancelPending(row); break;
        case DaemonRowButton::Remove: removeDaemon(row); break;
    }
}

void SidebarModel::openInNewWindow(int row)
{
    auto const* const shown = rowAt(row);
    if (shown == nullptr || shown->key.kind == SidebarRowKind::Hidden)
        return;
    _host.jumpTo(sidebarTargetOf(shown->key), JumpMode::NewWindow);
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
        case SidebarRowKind::Workspace: {
            // A workspace is toggled where it folds against its daemon's default.
            auto const workspace =
                WorkspaceKey { .daemon = key.daemon, .instance = key.instance, .workspace = key.workspace };
            auto const foldedByDefault = _state.foldsByDefault(key.daemon);
            if (collapsing != foldedByDefault)
                _state.toggled.insert(workspace);
            else
                _state.toggled.erase(workspace);
            break;
        }
        case SidebarRowKind::Tab: {
            auto const tab = TabKey {
                .workspace = { .daemon = key.daemon, .instance = key.instance, .workspace = key.workspace },
                .tab = key.tab
            };
            if (collapsing)
                _state.collapsedTabs.insert(tab);
            else
                _state.collapsedTabs.erase(tab);
            break;
        }
        case SidebarRowKind::Daemon:
            if (collapsing)
                _state.foldedDaemons.insert(key.daemon);
            else
                _state.foldedDaemons.erase(key.daemon);
            break;
        case SidebarRowKind::Pane:
        case SidebarRowKind::Hidden: return;
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
    auto const* const shown = ownRowAt(row);
    // Another daemon's rows are not the window's to rename.
    if (shown == nullptr)
        return;
    switch (shown->key.kind)
    {
        case SidebarRowKind::Workspace:
            _host.renameWorkspace(shown->key.workspace, name.toStdString());
            break;
        case SidebarRowKind::Tab:
            _host.renameTab(TabRef { .workspace = shown->key.workspace, .tab = shown->key.tab },
                            name.toStdString());
            break;
        case SidebarRowKind::Pane:
        case SidebarRowKind::Daemon:
        case SidebarRowKind::Hidden: break;
    }
}

void SidebarModel::markSeen(int row)
{
    auto const* const shown = ownRowAt(row);
    if (shown == nullptr)
        return;
    for (auto const pane: panesUnder(directory(), shown->key))
        _host.markSeen(pane);
    noteChanged();
}

QStringList SidebarModel::hoverLines(int row) const
{
    auto const* const shown = ownRowAt(row);
    if (shown == nullptr
        || (shown->key.kind != SidebarRowKind::Tab && shown->key.kind != SidebarRowKind::Pane))
        return {};
    return toQStringList(_host.hoverLines(shown->key));
}

void SidebarModel::openContextMenu(int row, QPointF windowPosition)
{
    // The window's menus act on its own rows, and on every daemon row; another daemon's other rows have none
    // yet.
    if (auto const* const shown = rowAt(row);
        shown != nullptr && (shown->key.kind == SidebarRowKind::Daemon || isOwn(shown->key)))
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

void SidebarModel::setScope(config::SidebarScope scope)
{
    if (scope == _state.scope)
        return;
    _state.scope = scope;
    refreshNow();
    emit scopeChanged();
}

void SidebarModel::setScope(int scope)
{
    setScope(scope >= 0 && static_cast<std::size_t>(scope) < config::SidebarScopeCount
                 ? static_cast<config::SidebarScope>(scope)
                 : config::SidebarScope::All);
}

std::optional<SidebarModel::Dragged> SidebarModel::draggedOf(QString const& payload) const
{
    auto const drag = parseNodeDrag(payload);
    // Ids are the host's own: another host's tab 3 or workspace 2 is not this one's.
    if (!drag || drag->host != _host.hostIdentity())
        return std::nullopt;
    auto const current = directory();
    auto const host = _host.hostMoves();
    auto const tabsOf = [&](uint64_t workspace) {
        auto const* const found = findWorkspace(current, workspace);
        return found != nullptr ? std::span<DirectoryTab const> { found->tabs }
                                : std::span<DirectoryTab const> {};
    };
    switch (drag->payload)
    {
        case DropPayload::Workspace: {
            auto const tabs = tabsOf(drag->workspace);
            if (tabs.size() != 1)
                return Dragged {
                    .dragged = { .subject = DragSubject::Workspace, .tab = {}, .share = {}, .host = host },
                    .node = TabRef {},
                    .workspace = drag->workspace
                };
            // A workspace of one tab joins a tab as that tab does.
            auto const only = TabRef { .workspace = drag->workspace, .tab = tabs.front().id };
            return Dragged { .dragged = { .subject = DragSubject::OneTabWorkspace,
                                          .tab = only,
                                          .share = WorkspaceShare::Whole,
                                          .host = host },
                             .node = only,
                             .workspace = drag->workspace };
        }
        case DropPayload::Tab:
        case DropPayload::Pane: {
            if (drag->sessions.empty())
                return std::nullopt;
            auto const pane = PaneKey { drag->sessions.front() };
            auto const held = tabHoldingPane(current, pane);
            if (!held)
                return std::nullopt;
            auto const tab = TabRef { .workspace = held->workspace->id, .tab = held->tab->id };
            auto const share =
                held->workspace->tabs.size() == 1 ? WorkspaceShare::Whole : WorkspaceShare::Part;
            // A pane that is its tab's only one is the tab, as the tab strip takes it.
            if (drag->payload == DropPayload::Tab || held->tab->panes.size() == 1)
                return Dragged {
                    .dragged = { .subject = DragSubject::Tab, .tab = tab, .share = share, .host = host },
                    .node = tab,
                    .workspace = 0
                };
            return Dragged { .dragged = { .subject = DragSubject::Pane,
                                          .tab = tab,
                                          .share = WorkspaceShare::Part,
                                          .host = host },
                             .node = pane,
                             .workspace = 0 };
        }
        case DropPayload::None:
        case DropPayload::Files:
        case DropPayload::Text: break;
    }
    return std::nullopt;
}

void SidebarModel::apply(Dragged const& dragged, SidebarDrop const& drop)
{
    std::visit(
        core::Overloaded {
            [&](ReorderWorkspace const& reorder) { _host.moveWorkspace(dragged.workspace, reorder.to); },
            [](Refuse const&) {},
            // Every other drop moves a node: into a workspace, into a tab, or into one of its own.
            [&](auto const& destination) { _host.moveNode(dragged.node, NodeDestination { destination }); },
        },
        drop);
}

void SidebarModel::drop(int row, double y, QString const& payload)
{
    // A drop on another daemon's row would name ids of that daemon's: nothing moves there.
    if (auto const* const shown = rowAt(row); shown != nullptr && !isOwn(shown->key))
        return;
    if (auto const dragged = draggedOf(payload))
        apply(*dragged, resolveSidebarDrop(_rows, row, y, dragged->dragged));
}

int SidebarModel::dropIndicator(int row, double y, QString const& payload) const
{
    auto const* const shown = rowAt(row);
    auto const dragged = shown == nullptr || isOwn(shown->key) ? draggedOf(payload) : std::nullopt;
    return std::to_underlying(dragged ? sidebarDropIndicator(_rows, row, y, dragged->dragged)
                                      : DropIndicator::Refused);
}

bool SidebarModel::takesNewWorkspaceDrop(QString const& payload) const
{
    auto const dragged = draggedOf(payload);
    return dragged && std::holds_alternative<IntoNewWorkspace>(resolveNewWorkspaceDrop(dragged->dragged));
}

void SidebarModel::dropOnNewWorkspace(QString const& payload)
{
    if (auto const dragged = draggedOf(payload))
        apply(*dragged, resolveNewWorkspaceDrop(dragged->dragged));
}

} // namespace contour::window
