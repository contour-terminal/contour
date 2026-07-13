// SPDX-License-Identifier: Apache-2.0
#include <contour/CommandHistory.h>
#include <contour/CommandPaletteModel.h>
#include <contour/FuzzyFilter.h>

#include <algorithm>
#include <ranges>
#include <utility>

namespace contour
{

namespace
{
    /// Title ordering for the alphabetical section.
    ///
    /// Compares on the title the user actually reads, not on the id — "Switch To Tab 3: vim" belongs
    /// under S, wherever its id happens to sort. Titles are ASCII and consistently capitalized (they
    /// are derived from the action names), so a plain byte comparison already reads as alphabetical.
    [[nodiscard]] bool byTitle(Command const& a, Command const& b) noexcept
    {
        return a.title < b.title;
    }
} // namespace

CommandPaletteModel::CommandPaletteModel(CommandHistory const& history, QObject* parent):
    QAbstractListModel { parent }, _history { history }
{
}

void CommandPaletteModel::setSources(std::vector<CommandSource const*> sources)
{
    _sources = std::move(sources);
}

void CommandPaletteModel::setShortcuts(std::unordered_map<std::string, std::string> shortcuts)
{
    _shortcuts = std::move(shortcuts);
}

void CommandPaletteModel::refresh()
{
    _commands = collectCommands(_sources);

    // Clearing the filter here rather than making the caller do it with setFilter({}): that would
    // rebuild the rows once against the OLD command list, only for this refresh to throw the result
    // away and rebuild them again. One rebuild, and no ordering trap for the caller.
    auto const hadFilter = !_filter.isEmpty();
    _filter.clear();

    rebuildRows();

    if (hadFilter)
        emit filterChanged();
}

void CommandPaletteModel::setFilter(QString const& filter)
{
    if (_filter == filter)
        return;

    _filter = filter;
    rebuildRows();
    emit filterChanged();
}

void CommandPaletteModel::rebuildRows()
{
    // A full reset rather than fine-grained insert/remove signals: the ordering changes wholesale on
    // every keystroke (a fuzzy re-rank can move any row anywhere), so computing a minimal diff would be
    // more work than redrawing a list of some ninety rows.
    beginResetModel();
    _rows.clear();

    auto const query = _filter.toStdString();

    if (query.empty())
    {
        // Recent first, newest first — but only the ids that still RESOLVE against the live command
        // set. That is what makes the history self-healing: delete a profile and its ChangeProfile
        // entry simply stops appearing, with no stale row and no migration of the stored file.
        for (auto const& id: _history.recent())
            if (auto const* command = commandById(id))
                _rows.push_back(Row { .command = command, .section = Section::Recent });

        auto const firstAll = _rows.size();
        for (auto const& command: _commands)
            _rows.push_back(Row { .command = &command, .section = Section::All });

        // Sort just the freshly-appended stretch, leaving the recent rows pinned above it.
        std::ranges::sort(_rows.begin() + static_cast<std::ptrdiff_t>(firstAll),
                          _rows.end(),
                          [](Row const& a, Row const& b) { return byTitle(*a.command, *b.command); });
    }
    else
    {
        // Rank every match, best first. Recency breaks ties, so of two equally good matches the one the
        // user reached for last time comes up under the cursor.
        struct Scored
        {
            Command const* command;
            int score;
            int recency; //!< Position in the MRU list; lower is more recent. Absent -> past the end.
        };

        auto const recencyOf = [this](std::string const& id) {
            // recent() is a span over at most `command_palette_recent_count` (5 by default) entries, so
            // this scan is cheaper than the index that would replace it.
            auto const recent = _history.recent();
            return static_cast<int>(std::ranges::distance(recent.begin(), std::ranges::find(recent, id)));
        };

        auto scored = std::vector<Scored> {};
        scored.reserve(_commands.size());

        for (auto const& command: _commands)
        {
            // Match against the title (what the user sees and therefore types at), falling back to the
            // id — so "splitv" still finds "Split Vertical" even though its title has a space there.
            auto score = fuzzyScore(query, command.title);
            if (!score)
                score = fuzzyScore(query, command.id);
            if (!score)
                continue;

            scored.push_back(
                Scored { .command = &command, .score = *score, .recency = recencyOf(command.id) });
        }

        std::ranges::stable_sort(scored, [](Scored const& a, Scored const& b) {
            if (a.score != b.score)
                return a.score > b.score;
            if (a.recency != b.recency)
                return a.recency < b.recency;
            return byTitle(*a.command, *b.command);
        });

        for (auto const& entry: scored)
            _rows.push_back(Row { .command = entry.command, .section = Section::All });
    }

    endResetModel();
}

Command const* CommandPaletteModel::commandById(std::string_view id) const noexcept
{
    auto const found = std::ranges::find_if(_commands, [&](auto const& command) { return command.id == id; });
    if (found == _commands.end())
        return nullptr;
    return &*found;
}

int CommandPaletteModel::rowCount(QModelIndex const& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(_rows.size());
}

QVariant CommandPaletteModel::data(QModelIndex const& index, int role) const
{
    auto const row = index.row();
    if (row < 0 || static_cast<std::size_t>(row) >= _rows.size())
        return {};

    auto const& entry = _rows[static_cast<std::size_t>(row)];

    switch (role)
    {
        case IdRole: return QString::fromStdString(entry.command->id);
        case TitleRole: return QString::fromStdString(entry.command->title);
        case DescriptionRole: return QString::fromStdString(entry.command->description);
        case ShortcutRole: {
            auto const shortcut = _shortcuts.find(entry.command->id);
            if (shortcut == _shortcuts.end())
                return QString {};
            return QString::fromStdString(shortcut->second);
        }
        case SectionRole: return static_cast<int>(entry.section);
        case SectionStartRole:
            // Answers the whole question the view asks — "draw a header above this row?" — rather than
            // half of it. A filtered list has no sections, so no row starts one; otherwise a header goes
            // above the first row and above any row whose section differs from its predecessor's. QML
            // therefore binds `visible: row.sectionStart` and never has to re-derive "is there a query?"
            // or look backwards from inside a virtualized delegate.
            return sectioned()
                   && (row == 0 || _rows[static_cast<std::size_t>(row) - 1].section != entry.section);
        default: return {};
    }
}

QHash<int, QByteArray> CommandPaletteModel::roleNames() const
{
    return {
        { IdRole, "commandId" },      { TitleRole, "title" },     { DescriptionRole, "description" },
        { ShortcutRole, "shortcut" }, { SectionRole, "section" }, { SectionStartRole, "sectionStart" },
    };
}

} // namespace contour
