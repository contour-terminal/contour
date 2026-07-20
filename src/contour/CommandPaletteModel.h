// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Command.h>
#include <contour/CommandCatalog.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <QtQmlIntegration/QtQmlIntegration>

namespace contour
{

class CommandHistory;

/// The list the command palette shows: every runnable command, filtered and ordered for display.
///
/// It owns no commands and no history — it is handed its sources and the app-wide CommandHistory, and
/// re-reads both on refresh(). That keeps the two orderings the palette has to get right in one place:
///
///   - no filter: the recently-used commands first (newest first, capped at the history's capacity),
///     then every command alphabetically. Two sections, so the user can either grab what they just
///     used or browse the whole set.
///   - a filter: ONE flat list, best fuzzy match first, recency breaking ties. Sections would be noise
///     here — the user has already said what they are looking for.
class CommandPaletteModel: public QAbstractListModel
{
    Q_OBJECT
    /// What the user typed. Writing it re-ranks the list (see the class comment).
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// Whether the list is currently sectioned (i.e. the filter is empty). QML hides the section
    /// headers when it is not, rather than re-deriving the same condition from the filter text.
    Q_PROPERTY(bool sectioned READ sectioned NOTIFY filterChanged)
    QML_ELEMENT
    QML_UNCREATABLE("Created by the window controller")

  public:
    /// Which section a row belongs to. Mirrored in QML by the `section` role.
    enum class Section : std::uint8_t
    {
        Recent = 0, //!< Pinned above: the user ran this recently.
        All = 1,    //!< The full alphabetical list.
    };

    enum class Roles : std::uint16_t
    {
        IdRole = Qt::UserRole + 1, //!< Stable command id; what runCommand() is called with.
        TitleRole,                 //!< Display name, e.g. "Split Vertical".
        DescriptionRole,           //!< What the command does (shown under the title).
        ShortcutRole,              //!< Rendered key binding ("Ctrl+Shift+E"), or empty if unbound.
        SectionRole,               //!< The row's Section, as an int.
        SectionStartRole,          //!< True on the FIRST row of a section, so QML draws one header for it.
        TitleMatchesRole,          //!< Matched title indices (UTF-16 code units) for highlighting.
    };

    /// @param history The app-wide most-recently-used list. Must outlive this model.
    /// @param parent  Qt parent.
    explicit CommandPaletteModel(CommandHistory const& history, QObject* parent = nullptr);

    /// Points this model at the sources it draws commands from.
    ///
    /// Held as raw pointers rather than copied, because the dynamic sources (tabs, profiles, layouts)
    /// must report LIVE state on each refresh(); a snapshot would go stale the moment a tab opened.
    /// Every source must outlive this model.
    ///
    /// @param sources The sources, in precedence order (see collectCommands()).
    void setSources(std::vector<CommandSource const*> sources);

    /// The shortcut to advertise per command id, from shortcutIndex().
    /// @param shortcuts Command id -> rendered shortcut.
    void setShortcuts(std::unordered_map<std::string, std::string> shortcuts);

    /// Re-queries the sources and rebuilds the list. Called each time the palette opens, so the
    /// dynamic rows (open tabs) and the recent section reflect the state the user is looking at.
    void refresh();

    /// The offered command whose id is @p id, or nullptr when none is.
    ///
    /// Searches everything the sources offer, not just the filtered rows — a command is the same
    /// command whether or not the current filter happens to be showing it.
    ///
    /// @param id The command id to look up.
    [[nodiscard]] Command const* commandById(std::string_view id) const noexcept;

    // {{{ QAbstractListModel
    [[nodiscard]] QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex()) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // }}}

    [[nodiscard]] QString filter() const { return _filter; }
    void setFilter(QString const& filter);

    [[nodiscard]] bool sectioned() const noexcept { return _filter.isEmpty(); }

  signals:
    void filterChanged();

  private:
    /// One displayed row: which command it shows, and which section it landed in.
    ///
    /// Points INTO _commands rather than copying the command: _commands is only ever reassigned by
    /// refresh(), which rebuilds every row immediately afterwards, so a row can never outlive what it
    /// points at. Copying instead would duplicate the whole command set (three strings apiece) on
    /// every keystroke, for nothing.
    struct Row
    {
        Command const* command;
        Section section;
        /// UTF-8 byte offsets into the command's title that the active filter matched, ascending. These
        /// are converted to UTF-16 code-unit indices at the TitleMatchesRole boundary (what QML bolds).
        /// Empty when there is no filter, or when the row matched only through its id (there is nothing in
        /// the visible title to highlight then).
        std::vector<int> titleMatches;
    };

    /// Rebuilds _rows from _commands, applying the current filter and ordering (see the class comment).
    void rebuildRows();

    CommandHistory const& _history;
    std::vector<CommandSource const*> _sources;
    std::unordered_map<std::string, std::string> _shortcuts;

    std::vector<Command> _commands; //!< Everything the sources offer; rebuilt by refresh().
    std::vector<Row> _rows;         //!< What is currently displayed; rebuilt by rebuildRows().
    QString _filter;
};

} // namespace contour
