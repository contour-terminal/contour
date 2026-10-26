// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/Command.hpp>
#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandPaletteTree.hpp>
#include <contour/command/Shortcut.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <QtQmlIntegration/QtQmlIntegration>

namespace contour::command
{
class CommandHistory;
}

namespace contour::window
{

/// The list the command palette shows: the grouped top level, or one parameterised action's level.
///
/// A Qt adapter over command::listPalette(), which owns every ordering decision: the recently used
/// commands and the groups when unfiltered, a ranking across the groups (children included) when
/// filtered, and a parent's children inside its level. This class holds the sources, the level the
/// user is in and the filter that level was entered from, and publishes the rows as roles.
class CommandPaletteModel: public QAbstractListModel
{
    Q_OBJECT
    /// What the user typed. Writing it re-ranks the list.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// Where the palette is: a command::PaletteDepth, as an int.
    Q_PROPERTY(int depth READ depth NOTIFY levelChanged)
    /// The title of the parent drilled into; empty at the top.
    Q_PROPERTY(QString levelTitle READ levelTitle NOTIFY levelChanged)
    /// The row QML selects after a level change: 0 after enter(), the parent's row after back().
    Q_PROPERTY(int anchorRow READ anchorRow NOTIFY levelChanged)
    /// command::PaletteEntryKind::DrillIn as an int, so QML compares the `kind` role by name.
    Q_PROPERTY(int drillInKind READ drillInKind CONSTANT)
    /// command::PaletteDepth::Top as an int, so QML compares `depth` by name.
    Q_PROPERTY(int topDepth READ topDepth CONSTANT)
    /// Whether the palette is on screen; the QML sets it as the palette shows and closes. A bool for QML
    /// (the QML-facing carve-out): while it holds, refresh() leaves the open palette's rows, level and query
    /// alone, so a second OpenCommandPalette does not empty the list under a typed query.
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)
    QML_ELEMENT
    QML_UNCREATABLE("Created by the window controller")

  public:
    enum class Roles : std::uint16_t
    {
        IdRole = Qt::UserRole + 1, //!< A command's id (what runCommand() takes); a parent's catalogue name.
        TitleRole,                 //!< What the row reads, e.g. "Split Vertical", "Change Profile · dark".
        DescriptionRole,           //!< What the command (or the parameterised action) does.
        ShortcutRole,              //!< Rendered key binding ("Ctrl+Shift+E"); empty if unbound or a parent.
        GroupTitleRole,            //!< The group's header on its first row; empty on every other row.
        TitleMatchesRole,          //!< Matched title indices (UTF-16 code units) for highlighting.
        KindRole,                  //!< The row's command::PaletteEntryKind, as an int.
        FooterHintsRole,           //!< The footer's key hints with this row selected, e.g. "↩ Run · ⎋ Close".
    };

    /// @param history   The app-wide most-recently-used list. Must outlive this model.
    /// @param sources   The sources, in precedence order (see collectCommands()). Held as pointers
    ///                  because the live sources (tabs, profiles) must report what is true at each
    ///                  refresh(); every source must outlive this model.
    /// @param hintStyle How the footer's key hints spell Enter and Escape.
    /// @param parent    Qt parent.
    CommandPaletteModel(command::CommandHistory const& history,
                        std::vector<command::CommandSource const*> sources,
                        command::ShortcutStyle hintStyle,
                        QObject* parent = nullptr);

    /// Re-queries the sources and rebuilds the tree, back at the top level with an empty filter; does
    /// nothing while the palette is shown.
    ///
    /// Called each time the palette opens, so the live rows and the recent group reflect the state the
    /// user is looking at, and every open starts at the top. One model reset.
    /// @param shortcuts Command id -> rendered shortcut, from shortcutIndex(); re-read on every open so a
    ///                  reloaded config's bindings show.
    void refresh(std::unordered_map<std::string, std::string> shortcuts);

    /// The offered command whose id is @p id, children included; nullptr when none is.
    ///
    /// Searches everything the sources offer, not just the listed rows.
    /// @param id The command id to look up.
    /// @return The command, or nullptr.
    [[nodiscard]] command::Command const* commandById(std::string_view id) const noexcept;

    /// Enters the level of the drill-in row @p row: keeps the current filter for back(), then clears it.
    /// Emits levelChanged() then filterChanged(). Does nothing on any other row or out of range.
    /// @param row The row to enter.
    Q_INVOKABLE void enter(int row);

    /// Leaves the level, restoring the filter it was entered from; anchorRow becomes the parent's row.
    /// Emits levelChanged() then filterChanged(). Does nothing at the top.
    Q_INVOKABLE void back();

    // {{{ QAbstractListModel
    [[nodiscard]] QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex()) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // }}}

    [[nodiscard]] QString filter() const { return _filter; }
    void setFilter(QString const& filter);

    [[nodiscard]] int depth() const noexcept { return static_cast<int>(paletteDepth()); }
    [[nodiscard]] QString levelTitle() const;
    [[nodiscard]] int anchorRow() const noexcept { return _anchorRow; }
    [[nodiscard]] int drillInKind() const noexcept
    {
        return static_cast<int>(command::PaletteEntryKind::DrillIn);
    }
    [[nodiscard]] int topDepth() const noexcept { return static_cast<int>(command::PaletteDepth::Top); }
    [[nodiscard]] bool shown() const noexcept { return _isShown; }
    /// @param shown Whether the palette is on screen (the property is the parameter: QML's carve-out).
    void setShown(bool shown);

  signals:
    void filterChanged();
    void levelChanged();
    void shownChanged();

  private:
    /// Where the palette is, from _level.
    [[nodiscard]] command::PaletteDepth paletteDepth() const noexcept;

    /// Rebuilds _rows from _tree for the current filter and level. One model reset.
    void rebuildRows();

    command::CommandHistory const& _history;
    std::vector<command::CommandSource const*> _sources;
    command::ShortcutStyle _hintStyle;
    std::unordered_map<std::string, std::string> _shortcuts;

    command::PaletteTree _tree;               //!< Everything the sources offer; rebuilt by refresh().
    std::vector<command::PaletteEntry> _rows; //!< What is listed; rebuilt by rebuildRows().
    std::optional<std::size_t> _level;        //!< The parent drilled into; none at the top.
    QString _filter;                          //!< The current query.
    QString _topFilter;                       //!< The top-level query a level was entered from.
    int _anchorRow = 0;                       //!< See anchorRow.
    bool _isShown = false;                    //!< See shown.
};

} // namespace contour::window
