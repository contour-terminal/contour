// SPDX-License-Identifier: Apache-2.0
#include <contour/command/CommandHistory.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/Utf16Offsets.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::window
{

CommandPaletteModel::CommandPaletteModel(command::CommandHistory const& history,
                                         std::vector<command::CommandSource const*> sources,
                                         command::ShortcutStyle hintStyle,
                                         QObject* parent):
    QAbstractListModel { parent },
    _history { history },
    _sources { std::move(sources) },
    _hintStyle { hintStyle },
    _shortcuts {},
    _tree {},
    _rows {},
    _level {},
    _filter {},
    _topFilter {}
{
}

void CommandPaletteModel::setShown(bool shown)
{
    if (_isShown == shown)
        return;
    _isShown = shown;
    emit shownChanged();
}

void CommandPaletteModel::refresh(std::unordered_map<std::string, std::string> shortcuts)
{
    // An open palette keeps what the user is looking at: its query, its level and its rows.
    if (_isShown)
        return;

    _shortcuts = std::move(shortcuts);
    _tree = command::buildPaletteTree(command::collectCommands(_sources));

    // Every open starts at the top with an empty query. Reset here rather than through back() and
    // setFilter({}), which would each rebuild the rows against the OLD tree first.
    // levelChanged is anchorRow's NOTIFY too, so it fires when either moves.
    auto const levelOrAnchorMoves = _level.has_value() || _anchorRow != 0;
    auto const hadFilter = !_filter.isEmpty();
    _level.reset();
    _filter.clear();
    _topFilter.clear();
    _anchorRow = 0;

    rebuildRows();

    if (levelOrAnchorMoves)
        emit levelChanged();
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

void CommandPaletteModel::enter(int row)
{
    if (_level || row < 0 || static_cast<std::size_t>(row) >= _rows.size())
        return;
    auto const& entry = _rows[static_cast<std::size_t>(row)];
    if (entry.kind != command::PaletteEntryKind::DrillIn)
        return;

    _level = entry.index;
    _topFilter = std::exchange(_filter, QString {});
    _anchorRow = 0;
    rebuildRows();

    emit levelChanged();
    emit filterChanged();
}

void CommandPaletteModel::back()
{
    if (!_level)
        return;

    auto const parent = *_level;
    _level.reset();
    _filter = std::exchange(_topFilter, QString {});
    rebuildRows();

    // The parent's row in the restored listing, so the user lands where they left.
    auto const found = std::ranges::find_if(_rows, [parent](command::PaletteEntry const& entry) {
        return entry.kind == command::PaletteEntryKind::DrillIn && entry.index == parent;
    });
    _anchorRow = found == _rows.end() ? 0 : static_cast<int>(std::ranges::distance(_rows.begin(), found));

    emit levelChanged();
    emit filterChanged();
}

QString CommandPaletteModel::levelTitle() const
{
    if (!_level)
        return {};
    return QString::fromStdString(_tree.parents[*_level].title);
}

command::PaletteDepth CommandPaletteModel::paletteDepth() const noexcept
{
    return _level ? command::PaletteDepth::Level : command::PaletteDepth::Top;
}

void CommandPaletteModel::rebuildRows()
{
    // A full reset rather than fine-grained insert/remove signals: a re-rank can move any row anywhere.
    beginResetModel();
    auto const filter = _filter.toStdString();
    _rows = command::listPalette(
        _tree, command::PaletteQuery { .filter = filter, .level = _level, .recent = _history.recent() });
    endResetModel();
    emit groupCountChanged();
}

int CommandPaletteModel::groupCount() const noexcept
{
    return static_cast<int>(std::ranges::count_if(
        _rows, [](command::PaletteEntry const& entry) { return !entry.groupTitle.empty(); }));
}

command::Command const* CommandPaletteModel::commandById(std::string_view id) const noexcept
{
    return _tree.find(id);
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
    auto const isCommand = entry.kind == command::PaletteEntryKind::Command;
    auto const* command = isCommand ? &_tree.commands[entry.index] : nullptr;
    auto const* parent = isCommand ? nullptr : &_tree.parents[entry.index];

    switch (static_cast<Roles>(role))
    {
        case Roles::IdRole: return QString::fromStdString(command ? command->id : parent->name);
        case Roles::TitleRole: return QString::fromStdString(entry.title);
        case Roles::DescriptionRole:
            return QString::fromStdString(command ? command->description : parent->description);
        case Roles::ShortcutRole: {
            if (!command)
                return QString {};
            auto const shortcut = _shortcuts.find(command->id);
            if (shortcut == _shortcuts.end())
                return QString {};
            return QString::fromStdString(shortcut->second);
        }
        case Roles::GroupTitleRole:
            return QString::fromUtf8(entry.groupTitle.data(),
                                     static_cast<qsizetype>(entry.groupTitle.size()));
        case Roles::TitleMatchesRole: {
            // FuzzyFilter reports UTF-8 byte offsets, QML indexes the title by UTF-16 code unit; translate
            // here, where the UTF-8 title is known. An offset inside a multibyte character is dropped.
            auto const utf16Matches = byteOffsetsToUtf16(entry.title, entry.titleMatches);
            auto matches = QVariantList {};
            matches.reserve(static_cast<qsizetype>(utf16Matches.size()));
            for (auto const position: utf16Matches)
                matches.append(position);
            return matches;
        }
        case Roles::KindRole: return static_cast<int>(entry.kind);
        case Roles::FooterHintsRole:
            // Per row rather than an invokable: a level change and a re-rank both reset the model, so the
            // selected row's hints are always the current ones.
            return QString::fromStdString(
                command::paletteFooterHints(entry.kind, paletteDepth(), _hintStyle));
        default: return {};
    }
}

QHash<int, QByteArray> CommandPaletteModel::roleNames() const
{
    return {
        { static_cast<int>(Roles::IdRole), "commandId" },
        { static_cast<int>(Roles::TitleRole), "title" },
        { static_cast<int>(Roles::DescriptionRole), "description" },
        { static_cast<int>(Roles::ShortcutRole), "shortcut" },
        { static_cast<int>(Roles::GroupTitleRole), "groupTitle" },
        { static_cast<int>(Roles::TitleMatchesRole), "titleMatches" },
        { static_cast<int>(Roles::KindRole), "kind" },
        { static_cast<int>(Roles::FooterHintsRole), "footerHints" },
    };
}

} // namespace contour::window
