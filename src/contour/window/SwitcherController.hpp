// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/cli/NewDaemon.hpp>
#include <contour/platform/OffThreadJobs.hpp>
#include <contour/session/WindowSwitching.hpp>
#include <contour/window/SwitcherRows.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtQuick/QQuickWindow>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QtQmlIntegration/QtQmlIntegration>
#include <vtworkspace/Primitives.hpp>

class QQmlEngine;

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
};

/// The daemon and workspace switcher popup's list and what choosing in it does: a Qt adapter over
/// switcherRows(), which owns every grouping, filtering and naming decision. It holds what the user typed,
/// the selection, which daemons are expanded and their listings, and the "New daemon…" prompt.
///
/// Choosing a row asks the window switcher and closes the popup (finished()); cancelling changes nothing
/// (cancelled()). Discovery, listings and a daemon's start answer later, on the GUI thread, each dropped
/// once the popup was opened again.
class SwitcherController: public QAbstractListModel
{
    Q_OBJECT
    /// What the user typed; writing it filters the list.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// The selected row, or -1 for an empty list.
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    /// Whether the popup asks for a new daemon's label. A QML-facing predicate, hence a bool.
    Q_PROPERTY(bool isAskingLabel READ isAskingLabel NOTIFY promptChanged)
    /// The group header of a pick: `Workspaces on admin`; empty for the whole switcher.
    Q_PROPERTY(QString pickTitle READ pickTitle NOTIFY openingChanged)
    /// Why the label typed cannot be used, shown under the field; empty when it can.
    Q_PROPERTY(QString labelError READ labelError NOTIFY labelChanged)
    /// The running daemon the label names, offered to switch to instead; empty for none.
    Q_PROPERTY(QString runningLabel READ runningLabel NOTIFY labelChanged)
    /// Whether a daemon is being started. A QML-facing predicate, hence a bool.
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY busyChanged)
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")

  public:
    /// The roles a row publishes.
    enum class Roles : std::uint16_t
    {
        TitleRole = Qt::UserRole + 1, ///< The row's name.
        DetailRole,                   ///< The line under it.
        TagRole,                      ///< `SSH` or `TLS` on a host; empty otherwise.
        GroupTitleRole,               ///< The group's header on its first row; empty on every other.
        KindRole,                     ///< `daemon`, `workspace`, `new-workspace` or `new-daemon`.
        ExpandedRole,                 ///< Whether a daemon row's workspaces are shown under it.
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

    /// Shows the popup afresh over a window: what opening it does. Clears the typed text, collapses every
    /// daemon but the window's own, which it lists, and looks for the local daemons again.
    /// @param opening Over which window, and what to show.
    void open(SwitcherOpening opening);

    /// Chooses the selected row with the opening's mode (Return).
    Q_INVOKABLE void activateCurrent();

    /// Chooses the selected row in a new window (Shift+Return).
    Q_INVOKABLE void openCurrent();

    /// Chooses @p row with the opening's mode (a tap).
    /// @param row The row.
    Q_INVOKABLE void activate(int row);

    /// Moves the selection by @p delta rows, stopping at either end.
    /// @param delta Rows to move; negative moves up.
    Q_INVOKABLE void step(int delta);

    /// Shows the selected daemon's workspaces under it, listing them (connecting a saved host) first.
    Q_INVOKABLE void expandCurrent();

    /// Hides the workspaces under the selected daemon, or under the daemon of the selected workspace.
    Q_INVOKABLE void collapseCurrent();

    /// Expands a collapsed daemon row, collapses an expanded one (a tap on its disclosure).
    /// @param row The row.
    Q_INVOKABLE void toggle(int row);

    /// Starts a daemon under the label @p label, then opens a new window on it; or, for a label that cannot
    /// be used or one already running, says so under the field (Return in the label prompt).
    /// @param label The label typed.
    Q_INVOKABLE void submitLabel(QString const& label);

    /// Checks @p label as it is typed: an error or an offer shows at once.
    /// @param label The label typed.
    Q_INVOKABLE void checkLabel(QString const& label);

    /// Switches the window to the running daemon the label named (the offer under the field).
    Q_INVOKABLE void switchToRunning();

    /// Drops the start in flight, if any, without closing anything: what closing the window does.
    Q_INVOKABLE void abandon();

    /// Gives up: nothing changes (Esc).
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
    [[nodiscard]] bool isAskingLabel() const noexcept { return _prompt == SwitcherPrompt::NewDaemon; }
    [[nodiscard]] QString pickTitle() const;
    [[nodiscard]] QString labelError() const { return _labelError; }
    [[nodiscard]] QString runningLabel() const { return _runningLabel; }
    [[nodiscard]] bool isBusy() const noexcept { return _isBusy; }

    /// @return The rows as shown now.
    [[nodiscard]] std::vector<SwitcherRow> const& rows() const noexcept { return _rows; }

  signals:
    void filterChanged();
    void currentIndexChanged();
    void openingChanged();
    void promptChanged();
    void labelChanged();
    void busyChanged();
    /// A choice was made: the popup's job is done.
    void finished();
    /// The user gave up.
    void cancelled();

  private:
    /// Starts looking for the local daemons; the answer adds their rows unless the popup was opened again.
    void discoverDaemons();

    /// Merges @p found into the daemons listed, keeping what is known of each one already there.
    void mergeLocalDaemons(std::vector<cli::DiscoveredDaemon> const& found);

    /// Lists the workspaces of daemon @p index, unless they are being listed.
    void listDaemon(std::size_t index);

    /// Rebuilds _rows for the current filter. One model reset; keeps the selection on @p keep when given.
    void rebuild(std::optional<session::SwitchTarget> const& keep);

    /// @return The target of the selected row, to keep the selection on across a rebuild.
    [[nodiscard]] std::optional<session::SwitchTarget> selectedTarget() const;

    /// Chooses @p row: asks the switch, or for "New daemon…" shows the label prompt.
    void choose(std::size_t row, session::SwitchMode mode);

    /// Asks the window switcher, and closes.
    void finishWith(session::SwitchTarget target, session::SwitchMode mode);

    /// Shows @p prompt.
    void setPrompt(SwitcherPrompt prompt);

    /// Shows @p error and @p running under the label field.
    void setLabelState(QString error, QString running);

    /// The worker's answer to a start: opens a window on the daemon, or says why not.
    void finishStart(std::uint64_t opening,
                     std::string const& label,
                     std::expected<cli::NamedDaemonStart, std::string> result);

    void setBusy(bool busy);

    SwitcherServices _services;
    SwitcherOpening _opening;
    std::vector<SwitcherDaemon> _daemons; ///< What the list is made of, in display order.
    std::vector<SwitcherRow> _rows;
    std::vector<cli::DiscoveredDaemon> _localDaemons; ///< What discovery found; empty while it looks.
    std::uint64_t _openings = 0; ///< Counts openings; an answer for an earlier one is dropped.
    QString _filter;
    int _current = -1;
    SwitcherPrompt _prompt = SwitcherPrompt::Browse;
    QString _labelError;
    QString _runningLabel;
    bool _isBusy = false;
    /// The discoveries and starts under way, their answers handed back here; joined first, as the controller
    /// goes, and the ones that ended as the next one starts.
    platform::OffThreadJobs _jobs;
};

/// Builds the switcher popup's window on @p engine.
/// @param engine The app's QML engine.
/// @param switcher What the window shows; borrowed, it outlives the window.
/// @return The window, not yet shown; null when Switcher.qml did not load (the errors are logged).
[[nodiscard]] std::unique_ptr<QQuickWindow> loadSwitcherWindow(QQmlEngine& engine,
                                                               SwitcherController& switcher);

} // namespace contour::window
