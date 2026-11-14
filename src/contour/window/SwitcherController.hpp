// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/cli/NewDaemon.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/command/SidebarMenus.hpp>
#include <contour/command/SwitcherMenus.hpp>
#include <contour/config/Actions.hpp>
#include <contour/platform/OffThreadJobs.hpp>
#include <contour/session/WindowSwitching.hpp>
#include <contour/window/SwitcherKeys.hpp>
#include <contour/window/SwitcherRows.hpp>
#include <contour/window/SwitcherText.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QVariantList>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QtQmlIntegration/QtQmlIntegration>
#include <vtworkspace/Primitives.hpp>

namespace contour::window
{

/// Told once, on the GUI thread, a daemon's workspaces as the popup lists them, or why they could not be
/// listed.
using SwitcherListed = std::function<void(std::expected<std::vector<SwitcherWorkspace>, std::string>)>;

/// What the switcher popup reaches outside itself through. Every member is something a test replaces.
struct SwitcherServices
{
    /// Finds the local daemons; its discover() runs on a worker thread whenever the popup opens, so a socket
    /// that never answers cannot freeze it. Shared, outlives the controller's workers.
    std::shared_ptr<cli::IDaemonDiscovery> discovery;
    /// The saved hosts, by name, each with its transport tag (`SSH`, `TLS`). Called on the GUI thread.
    std::function<std::vector<SwitcherDaemon>()> savedHosts;
    /// Lists a daemon's workspaces for the window the popup is over, connecting to a saved host not yet
    /// connected. Called on the GUI thread; never blocks, and tells its answer later, on the GUI thread.
    std::function<void(
        vtworkspace::WorkspaceId window, session::SwitchTarget const& daemon, SwitcherListed done)>
        list;
    /// Switches the window, or opens a new one (session::WindowSwitching::request()). Called on the GUI
    /// thread.
    std::function<void(
        vtworkspace::WorkspaceId window, session::SwitchTarget target, session::SwitchMode mode)>
        request;
    /// Prepares starting a local daemon under a label ("New daemon…"). Called on the GUI thread; the job it
    /// returns runs on a worker thread and holds all it needs.
    std::function<std::function<std::expected<cli::NamedDaemonStart, std::string>()>(
        std::string const& label)>
        startDaemon;
    /// Creates a workspace labelled @c label on a daemon (`workspace.create`), then switches the window to it
    /// or opens it in a new one. Called on the GUI thread; never blocks.
    std::function<void(vtworkspace::WorkspaceId window,
                       session::SwitchTarget const& daemon,
                       std::string label,
                       session::SwitchMode mode)>
        createWorkspace {};
    /// The wall clock another daemon's times are measured against.
    std::function<std::chrono::system_clock::time_point()> now {};
    /// How the footer, the row menu and the label prompt spell keys.
    command::ShortcutStyle style = command::ShortcutStyle::Text;
    /// The chord bound to ShowSwitcher, spelled in @c style, from the bindings as they are now; empty while
    /// nothing binds it. Read as the popup opens: `All daemons…` shows it.
    std::function<std::string()> showSwitcherShortcut {};
    /// The sidebar's view of one of the window's own daemon's workspaces, by its daemon id, for its row menu.
    std::function<command::WorkspaceMenuState(vtworkspace::WorkspaceId window, uint64_t workspace)>
        workspaceMenu {};
    /// Runs one of the sidebar's workspace menu rows on that workspace, through the sidebar's menu path.
    std::function<void(vtworkspace::WorkspaceId window, uint64_t workspace, actions::Action const& action)>
        runWorkspaceAction {};
};

/// Who chose the selected row: nobody yet, or the user.
enum class SwitcherSelectionOrigin : uint8_t
{
    Opening = 0, ///< The popup's own choice (switcherInitialSelection); it follows the list as it fills.
    User,        ///< The user moved it (arrows, a pick, a level left); a rebuild keeps it on its row.
};

/// The daemon and workspace switcher popup's list and what choosing in it does: a Qt adapter over the pure
/// decisions of SwitcherRows (the rows of each level), SwitcherKeys (what a key does), SwitcherText (what a
/// row and the footer say) and SwitcherMenus (a row's menu). It holds what the user typed, the level shown,
/// the selection, the daemons and their listings, and the "New Daemon…" start in flight.
///
/// The window's own daemon's workspaces come from its sidebar (SwitcherOpening::windowWorkspaces, and
/// updateWindowWorkspaces() while open); every other local daemon is listed as it opens, and a saved host
/// only once drilled into. Choosing a row asks the window switcher and closes the popup (finished());
/// cancelling changes nothing (cancelled()). Discovery, listings and a daemon's start answer later, on the
/// GUI thread, each dropped once the popup was opened again.
class SwitcherController: public QAbstractListModel
{
    Q_OBJECT
    /// What the user typed; writing it filters the list. On the New Daemon level, the label.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// The selected row, or -1 for an empty list.
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    /// The level shown, a SwitcherLevel's underlying value.
    Q_PROPERTY(int level READ level NOTIFY levelChanged)
    /// The field's accessible name on the level shown: what it is for there.
    Q_PROPERTY(QString fieldName READ fieldName NOTIFY presentationChanged)
    /// The level chip before the field: the daemon drilled into, `New Daemon`, or empty.
    Q_PROPERTY(QString levelTitle READ levelTitle NOTIFY presentationChanged)
    /// The field's drawn hint.
    Q_PROPERTY(QString hintText READ hintText NOTIFY presentationChanged)
    /// The empty state, while what was typed matches no workspace or daemon; empty otherwise.
    Q_PROPERTY(QString emptyText READ emptyText NOTIFY presentationChanged)
    /// The footer's description of what Return does on the selected row.
    Q_PROPERTY(QString footerDescription READ footerDescription NOTIFY presentationChanged)
    /// The footer's key hints for the selected row.
    Q_PROPERTY(QString footerHints READ footerHints NOTIFY presentationChanged)
    /// The footer's hints for every width, widest first (switcherFooterHintSets).
    Q_PROPERTY(QStringList footerHintSets READ footerHintSets NOTIFY presentationChanged)
    /// Whether other local daemons are still looked for. A QML-facing predicate, hence a bool.
    Q_PROPERTY(bool isDiscovering READ isDiscovering NOTIFY discoveringChanged)
    /// Whether a daemon is being started. A QML-facing predicate, hence a bool.
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY busyChanged)
    /// Whether the New Daemon level is shown, which draws the label's body. A QML-facing predicate, hence a
    /// bool.
    Q_PROPERTY(bool isAskingLabel READ isAskingLabel NOTIFY levelChanged)
    /// What a label is, over the verdict.
    Q_PROPERTY(QString labelCaption READ labelCaption CONSTANT)
    /// The caption over the equivalent command.
    Q_PROPERTY(QString labelCommandCaption READ labelCommandCaption CONSTANT)
    /// The verdict's glyph, a TabBadgeKind's underlying value: Done for an available label, Error for one
    /// that cannot start, None while nothing is typed.
    Q_PROPERTY(int labelVerdictGlyph READ labelVerdictGlyph NOTIFY labelChanged)
    /// What the label typed amounts to, a LabelVerdict's underlying value.
    Q_PROPERTY(int labelVerdict READ labelVerdict NOTIFY labelChanged)
    /// The verdict line under the label, or why a start failed.
    Q_PROPERTY(QString labelMessage READ labelMessage NOTIFY labelChanged)
    /// The one action row's title; empty for none.
    Q_PROPERTY(QString labelAction READ labelAction NOTIFY labelChanged)
    /// `contour daemon --label ‹label›` for an available label; empty otherwise.
    Q_PROPERTY(QString labelCommand READ labelCommand NOTIFY labelChanged)
    /// How many group headers the rows draw: the rows that open a group. With every row the same height, it
    /// gives the list's height without laying the list out (PickerPopup sizes itself from it).
    Q_PROPERTY(int groupCount READ groupCount NOTIFY groupCountChanged)
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")

  public:
    /// The roles a row publishes.
    enum class Roles : std::uint16_t
    {
        TitleRole = Qt::UserRole + 1, ///< What the row is called.
        TitleMatchesRole,             ///< The UTF-16 indices of the title the filter matched.
        GroupTitleRole,               ///< The group's header on its first row; empty on every other.
        KindRole,                     ///< A SwitcherRowKind's underlying value.
        GlyphRole,                    ///< The status glyph, a TabBadgeKind's underlying value.
        DetailTextRole,               ///< The detail's proportional text.
        DetailSeparatorRole,          ///< Between the detail's text and its command; shown only with it.
        DetailCommandRole,            ///< The detail's command, drawn monospace; empty for none.
        DetailTailRole,               ///< The detail's text after the command.
        PillTextRole,                 ///< What the pill at its end says; empty for none.
        PillKindRole,                 ///< A SwitcherPill's underlying value.
        AccessibleDescriptionRole,    ///< Its kind and status in words, for assistive technology.
        DrillsInRole,                 ///< Whether it leads into a level: it wears the drill-in glyph.
        OpensInNewWindowRole,         ///< Whether it can open in a new window: it wears that button.
        NewWindowNameRole,            ///< That button's accessible name: `Open docs in a new window`.
    };

    /// @param services What the popup reaches outside itself through.
    /// @param parent Qt ownership.
    explicit SwitcherController(SwitcherServices services, QObject* parent = nullptr);

    /// Waits for the workers still running.
    ~SwitcherController() override;

    SwitcherController(SwitcherController const&) = delete;
    SwitcherController& operator=(SwitcherController const&) = delete;
    SwitcherController(SwitcherController&&) = delete;
    SwitcherController& operator=(SwitcherController&&) = delete;

    /// Shows the popup afresh over a window: what opening it does. Clears the typed text, takes the window's
    /// own daemon's workspaces from @p opening, and (for every daemon) looks for the local daemons again and
    /// lists each one found.
    /// @param opening Over which window, from where, and what to show.
    void open(SwitcherOpening opening);

    /// Replaces the window's own daemon's workspaces (its sidebar rebuilt), keeping the selection on its row.
    /// @param workspaces The workspaces, with their status, in the daemon's order.
    void updateWindowWorkspaces(std::vector<SwitcherWorkspace> workspaces);

    /// Does what @p key does in the switcher (SwitcherKeyRows).
    /// @param key A Qt::Key.
    /// @param modifiers The Qt::KeyboardModifiers held.
    /// @param caret The field's cursor position.
    /// @return Whether the switcher took the key; the field takes it otherwise. A QML-facing answer, hence a
    ///         bool.
    Q_INVOKABLE bool handleKey(int key, int modifiers, int caret);

    /// Chooses @p row as Return does: switch (in the opening's mode), create, or drill in.
    /// @param row The row.
    Q_INVOKABLE void activate(int row);

    /// Chooses @p row as Shift+Return does: in a new window, where the row can; else as activate().
    /// @param row The row.
    Q_INVOKABLE void activateInNewWindow(int row);

    /// Moves the selection by @p delta rows, stopping at either end.
    /// @param delta Rows to move; negative moves up.
    Q_INVOKABLE void step(int delta);

    /// The menu of a workspace row (command::buildSwitcherRowMenu), as ActionContextMenu draws it; empty for
    /// any other row. What triggerRowMenu() runs is pinned to the row it was built for.
    /// @param row The row.
    /// @return The menu rows.
    Q_INVOKABLE QVariantList rowMenu(int row);

    /// Runs the entry @p id of the menu rowMenu() built last.
    /// @param row The row the menu was opened over; the pinned row is used, wherever a rebuild moved it.
    /// @param id The entry's actionId.
    Q_INVOKABLE void triggerRowMenu(int row, int id);

    /// Takes @p label as typed on the New Daemon level: the verdict follows at once.
    /// @param label The label typed.
    Q_INVOKABLE void checkLabel(QString const& label);

    /// Acts on @p label (Return on the New Daemon level): starts a daemon under an available one, switches to
    /// one already running; does nothing for an empty or invalid one, whose verdict says why.
    /// @param label The label typed.
    /// @param mode A session::SwitchMode's underlying value: switch this window, or open a new one.
    Q_INVOKABLE void submitLabel(QString const& label, int mode);

    /// Drops the start in flight, if any, without closing anything: what closing the window does.
    Q_INVOKABLE void abandon();

    /// Gives up: nothing changes (Escape at the top level).
    Q_INVOKABLE void cancel();

    // {{{ QAbstractListModel
    [[nodiscard]] QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex()) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // }}}

    [[nodiscard]] QString filter() const { return _filter; }
    void setFilter(QString const& filter);
    [[nodiscard]] int currentIndex() const noexcept { return _current; }
    void setCurrentIndex(int index);
    [[nodiscard]] int level() const noexcept { return static_cast<int>(_level); }
    [[nodiscard]] QString fieldName() const { return _presentation.fieldName; }
    [[nodiscard]] QString levelTitle() const { return _presentation.levelTitle; }
    [[nodiscard]] QString hintText() const { return _presentation.hintText; }
    [[nodiscard]] QString emptyText() const { return _presentation.emptyText; }
    [[nodiscard]] QString footerDescription() const { return _presentation.footerDescription; }
    [[nodiscard]] QString footerHints() const { return _presentation.footerHintSets.value(0); }
    [[nodiscard]] QStringList footerHintSets() const { return _presentation.footerHintSets; }
    [[nodiscard]] bool isDiscovering() const noexcept { return _discovery == SwitcherDiscovery::Running; }
    [[nodiscard]] bool isBusy() const noexcept { return _isBusy; }
    [[nodiscard]] bool isAskingLabel() const noexcept { return _level == SwitcherLevel::NewDaemon; }
    [[nodiscard]] QString labelCaption() const;
    [[nodiscard]] QString labelCommandCaption() const;
    [[nodiscard]] int labelVerdictGlyph() const noexcept;
    [[nodiscard]] int labelVerdict() const noexcept { return static_cast<int>(_labelState.verdict); }
    [[nodiscard]] QString labelMessage() const;
    [[nodiscard]] QString labelAction() const { return QString::fromStdString(_labelState.actionTitle); }
    [[nodiscard]] QString labelCommand() const
    {
        return QString::fromStdString(_labelState.equivalentCommand);
    }
    [[nodiscard]] int groupCount() const noexcept;

    /// @return The rows as shown now.
    [[nodiscard]] std::vector<SwitcherRow> const& rows() const noexcept { return _rows; }
    /// @return The daemons the rows are made of, in display order.
    [[nodiscard]] std::vector<SwitcherDaemon> const& daemons() const noexcept { return _daemons; }
    /// @return Every daemon, or one, at the top level.
    [[nodiscard]] SwitcherScope scope() const noexcept { return _scope; }

  signals:
    void filterChanged();
    void currentIndexChanged();
    /// The level's texts (its field name too) or the footer changed.
    void presentationChanged();
    /// The level shown changed.
    void levelChanged();
    /// Discovery started or settled.
    void discoveringChanged();
    void labelChanged();
    void busyChanged();
    /// The rows were rebuilt, and with them their groups.
    void groupCountChanged();
    /// Menu or Shift+F10 asked for the selected row's menu.
    /// @param row The row.
    void rowMenuRequested(int row);
    /// A choice was made: the popup's job is done.
    void finished();
    /// The user gave up.
    void cancelled();

  private:
    /// What the field, the chip and the footer show, decided on each rebuild and selection move.
    struct Presentation
    {
        QString fieldName; ///< Names the level, its title included: notified with it.
        QString levelTitle;
        QString hintText;
        QString emptyText;
        QString footerDescription;
        QStringList footerHintSets; ///< Widest first; the first is the footer's whole hints.

        bool operator==(Presentation const&) const = default;
    };

    /// Where the user was before entering a level, or widening the chip: what Back returns to.
    struct Return
    {
        std::string filter;                          ///< What was typed there.
        std::optional<SwitcherRowIdentity> selected; ///< The row that was selected.
    };

    /// The row menu last built, and what its entries do.
    struct RowMenu
    {
        std::optional<SwitcherRowIdentity> row;           ///< The row it was built for.
        std::vector<command::SwitcherMenuChoice> choices; ///< Its leading entries, by actionId.
        actions::ActionList actions;                      ///< The sidebar's entries, by actionId - choices.
    };

    [[nodiscard]] SwitcherInputs inputs() const;
    [[nodiscard]] SwitcherView view() const;
    [[nodiscard]] SwitcherKeyContext keyContext(int caret) const;
    [[nodiscard]] SwitcherRow const* rowAt(int row) const noexcept;
    [[nodiscard]] std::optional<SwitcherRowIdentity> selectedIdentity() const;
    /// @return The workspace a workspace row stands for; null for another row, or one no longer listed.
    [[nodiscard]] SwitcherWorkspace const* workspaceOf(SwitcherRow const& row) const;

    /// Looks for the local daemons for the opening under way. A discovery still running from an earlier
    /// opening is not started again: its answer serves this opening instead. An answer adds its rows unless
    /// the opening it serves has ended by then.
    void discoverDaemons();

    /// Merges @p found into the daemons listed, keeping what is known of each one already there, then lists
    /// every local one but the window's.
    void mergeLocalDaemons(std::vector<cli::DiscoveredDaemon> const& found);

    /// Lists the workspaces of daemon @p index, unless they are being listed.
    void listDaemon(std::size_t index);

    /// @param address A daemon's address.
    /// @return Its index in _daemons; _daemons.size() for one not there.
    [[nodiscard]] std::size_t indexOf(session::SwitchTarget const& address) const;

    /// Shows @p level, telling QML when it changed.
    void setLevel(SwitcherLevel level);

    /// Sets whether other daemons are looked for, telling QML when it changed.
    void setDiscovery(SwitcherDiscovery discovery);

    /// Drops the start in flight, if any: its answer is no longer wanted.
    void dropStart();

    /// @return What @p row is to assistive technology: its kind, status, place and detail, in words.
    [[nodiscard]] QString accessibleDescriptionOf(SwitcherRow const& row) const;

    /// Rebuilds the rows: in place when they are the same rows, else in one model reset; the selection stays
    /// on @p keep while the user holds it.
    void rebuild(std::optional<SwitcherRowIdentity> const& keep);

    /// Decides the texts the field, the chip and the footer show, telling QML when they changed.
    void updatePresentation();

    /// @return What the field is called on the level shown.
    /// @param levelTitle The level's title, which a daemon's level names it by.
    [[nodiscard]] QString fieldNameOf(QString const& levelTitle) const;

    /// Enters the level the drill row @p row leads to, remembering where the user was.
    void drillIn(SwitcherRow const& row);

    /// Leaves the level (or narrows the widened chip), back to where the user was.
    void back();

    /// Creates a workspace from New Workspace @p row: labelled with what was typed, if anything.
    void createWorkspace(SwitcherRow const& row, session::SwitchMode mode);

    /// Asks the window switcher, and closes.
    void finishWith(session::SwitchTarget target, session::SwitchMode mode);

    /// Sets the filter, leaving the selection's origin alone.
    void replaceFilter(std::string text);

    /// The worker's answer to a start: switches to the daemon, or says why not.
    void finishStart(std::uint64_t start,
                     std::string const& label,
                     session::SwitchMode mode,
                     std::expected<cli::NamedDaemonStart, std::string> result);

    void setBusy(bool busy);

    /// Whether a discovery is under way: at most one is, however often the popup is reopened.
    enum class DiscoveryJob : uint8_t
    {
        Idle = 0,
        Running,
    };

    SwitcherServices _services;
    SwitcherOpening _opening;
    std::vector<SwitcherDaemon> _daemons; ///< What the list is made of, in display order.
    std::size_t _windowDaemon = 0;        ///< The window's own daemon, an index into _daemons.
    /// The daemon of the OneDaemon scope (the window's, or a pick's) and of the Daemon level, by address: a
    /// discovery reorders _daemons while either is shown, so each is found again as it is read (view()).
    session::SwitchTarget _scopeAddress;
    session::SwitchTarget _levelAddress;
    SwitcherLevel _level = SwitcherLevel::Top;
    SwitcherScope _scope = SwitcherScope::Everything;
    SwitcherDiscovery _discovery = SwitcherDiscovery::Settled;
    std::vector<Return> _returns; ///< Where Back goes, innermost last.
    std::vector<SwitcherRow> _rows;
    std::uint64_t _openings = 0; ///< Counts openings; an answer for an earlier one is dropped.
    DiscoveryJob _discoveryJob = DiscoveryJob::Idle;
    std::uint64_t _discoveryFor = 0; ///< The opening the running discovery's answer serves.
    std::uint64_t _listings = 0;     ///< Counts listings; a host's answer is dropped once backed out of.
    std::uint64_t _starts = 0; ///< Counts starts; one left (or relabelled) before it answered is dropped.
    /// The listing each daemon waits for, by address; a host left before it answered has none.
    std::vector<std::pair<session::SwitchTarget, std::uint64_t>> _pendingListings;
    QString _filter;
    std::string _filterText; ///< _filter as UTF-8, which the view reads.
    int _current = -1;
    SwitcherSelectionOrigin _selectionOrigin = SwitcherSelectionOrigin::Opening;
    Presentation _presentation;
    SwitcherLabelState _labelState;
    std::string _labelFailure;         ///< Why the last start failed; cleared as the label changes.
    std::string _showSwitcherShortcut; ///< SwitcherServices::showSwitcherShortcut, as of the opening.
    bool _isBusy = false;              ///< @see isBusy(); the QML-facing predicate's member.
    RowMenu _rowMenu;
    /// The discoveries and starts under way, their answers handed back here; joined first, as the controller
    /// goes, and the ones that ended as the next one starts.
    platform::OffThreadJobs _jobs;
};

} // namespace contour::window
