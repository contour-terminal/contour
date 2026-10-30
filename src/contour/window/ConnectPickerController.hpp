// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/cli/ConnectList.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/config/LaunchTarget.hpp>

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
#include <thread>
#include <vector>

#include <QtQmlIntegration/QtQmlIntegration>

class QQmlEngine;

namespace contour::window
{

/// Whether a plain launch asks where to connect first (`connect_picker_on_start`).
enum class StartPrompt : uint8_t
{
    Never = 0, ///< A plain launch opens its window at once.
    Ask,       ///< A plain launch shows the Connect picker first.
};

/// What the Connect picker reaches outside itself through: the probes, the process it starts, the Settings
/// window and the stores. Every member is something a test replaces.
struct ConnectPickerServices
{
    /// The live configuration: the saved hosts, and whether a launch asks. Called on the GUI thread.
    std::function<config::Config const&()> config;
    /// Prepares a probe of the local daemon, whenever the picker opens or refreshes. Called on the GUI
    /// thread, where the configuration is read; the job it returns runs on a worker thread, so a daemon that
    /// never answers cannot freeze the picker.
    std::function<std::function<cli::LocalDaemonState()>()> daemon;
    /// Prepares the check of whether a target answers, before any window is spent on it. Called on the GUI
    /// thread, where the configuration is read; the job it returns runs on a worker thread and holds all it
    /// needs. The job answers nothing for a target that cannot fail ahead of time (the daemon starts on
    /// connect, this window only needs nothing), the transport's own classified words for a host.
    std::function<std::function<std::expected<void, std::string>()>(config::LaunchTarget const&)> check;
    /// Starts the connection: a process of its own running `contour connect TARGET`. Called on the GUI
    /// thread.
    std::function<std::expected<void, std::string>(config::LaunchTarget const&)> launch;
    /// Shows the Settings window on a page (`section:hosts`, `host:NAME`). Called on the GUI thread.
    std::function<void(std::string const& page)> showSettingsPage;
    /// Stores the "Ask every time Contour starts" switch. Called on the GUI thread.
    std::function<void(StartPrompt)> setStartPrompt;
    /// Where the last choice is remembered; borrowed, outlives the controller.
    config::GuiStateStore& state;
    /// The modifier that, with a digit, picks a row.
    cli::QuickSelectKey quickKey = cli::QuickSelectKey::Alt;
};

/// The Connect picker's list and what it does: a Qt adapter over cli::connectRows(), which owns every
/// grouping, filtering and numbering decision. It holds the typed text, the selection, the failed row and the
/// connection in flight, and publishes the rows as roles.
///
/// Choosing a host checks it on a worker thread first and starts the connection only when it answers, so a
/// failure is shown under its row, in the words the transport gave, with the picker still open.
class ConnectPickerController: public QAbstractListModel
{
    Q_OBJECT
    /// What the user typed; writing it filters the list and renumbers its rows.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// The selected row, or -1 for an empty list.
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    /// Whether a connection is being checked. A QML-facing predicate, hence a bool.
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY busyChanged)
    /// Why the last connection failed; empty when none did. Shown under the row it belongs to.
    Q_PROPERTY(QString failure READ failure NOTIFY failureChanged)
    /// The text of the target that failed, the row `failure` belongs to; empty when none did.
    Q_PROPERTY(QString failedTarget READ failedTarget NOTIFY failureChanged)
    /// Whether the failed target is a saved host, so that "Edit Host…" has a host to edit. A QML-facing
    /// predicate.
    Q_PROPERTY(bool failureCanEditHost READ failureCanEditHost NOTIFY failureChanged)
    /// Whether a plain launch asks first. The footer's switch; QML-facing, hence a bool.
    Q_PROPERTY(bool askEveryTime READ askEveryTime WRITE setAskEveryTime NOTIFY askEveryTimeChanged)
    /// The modifier a quick-select shortcut names in a QML key sequence: `Alt`, or `Meta` (Command).
    Q_PROPERTY(QString quickModifier READ quickModifier CONSTANT)
    /// The modifier of the field-focus shortcut: `Ctrl`, or `Meta` (Command).
    Q_PROPERTY(QString focusModifier READ focusModifier CONSTANT)
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
        QuickRole,                    ///< The quick-select key (`⌘3`, `Alt+3`); empty past the ninth row.
        TargetRole,                   ///< The target's text, as `contour connect` reads it.
        FailureRole,                  ///< The connection error under this row; empty on every other.
    };

    /// @param services What the picker reaches outside itself through.
    /// @param parent Qt ownership.
    explicit ConnectPickerController(ConnectPickerServices services, QObject* parent = nullptr);

    /// Waits for a check still running.
    ~ConnectPickerController() override;

    ConnectPickerController(ConnectPickerController const&) = delete;
    ConnectPickerController& operator=(ConnectPickerController const&) = delete;
    ConnectPickerController(ConnectPickerController&&) = delete;
    ConnectPickerController& operator=(ConnectPickerController&&) = delete;

    /// Probes the daemon again, clears the typed text and any failure, and selects the last used target: what
    /// opening the picker does.
    void refresh();

    /// Connects to the row @p row: the daemon and this window only start at once, a host is checked first.
    /// Does nothing while a check runs or for a row out of range.
    /// @param row The row.
    Q_INVOKABLE void activate(int row);

    /// Connects to the selected row.
    Q_INVOKABLE void activateCurrent();

    /// Connects to the row the digit picks (Alt+digit, or ⌘digit); does nothing past the last row.
    /// @param digit 1 to 9.
    Q_INVOKABLE void quickSelect(int digit);

    /// Moves the selection by @p delta rows, stopping at either end.
    /// @param delta Rows to move; negative moves up.
    Q_INVOKABLE void step(int delta);

    /// Connects again to the target that failed.
    Q_INVOKABLE void retry();

    /// Opens Settings on the failed host's page.
    Q_INVOKABLE void editFailedHost() const;

    /// Opens Settings on the Hosts section.
    Q_INVOKABLE void manageHosts() const;

    /// Drops the connection being checked, if any, without closing anything: what closing the window does.
    Q_INVOKABLE void abandon();

    /// Gives up: the host window closes, and the app quits if it was the only one.
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
    [[nodiscard]] bool isBusy() const noexcept { return _isBusy; }
    [[nodiscard]] QString failure() const { return _failure; }
    [[nodiscard]] QString failedTarget() const { return _failedTarget; }
    [[nodiscard]] bool failureCanEditHost() const;
    [[nodiscard]] bool askEveryTime() const;
    /// @param ask Whether a plain launch asks first (the property is the parameter: QML's carve-out).
    void setAskEveryTime(bool ask);
    [[nodiscard]] QString quickModifier() const;
    [[nodiscard]] QString focusModifier() const;

  signals:
    void filterChanged();
    void currentIndexChanged();
    void busyChanged();
    void failureChanged();
    void askEveryTimeChanged();
    /// A connection was started: the picker's job is done.
    void connected();
    /// The user gave up.
    void cancelled();

  private:
    /// Starts probing the daemon; its answer replaces the row's "checking…" unless a newer probe or a
    /// cancellation has overtaken it.
    void probeDaemon();

    /// Rebuilds _rows for the current filter. One model reset; keeps the selection on @p keep when given.
    void rebuild(std::optional<std::string> keep);

    /// Starts the connection to @p target, checking it first when it is a host.
    void connectTo(config::LaunchTarget const& target);

    /// The worker's answer: either launches, or shows @p failure under the row.
    void finishCheck(std::uint64_t attempt,
                     config::LaunchTarget const& target,
                     std::expected<void, std::string> result);

    /// Starts the connection and remembers the target.
    void launch(config::LaunchTarget const& target);

    /// Shows @p reason under the row of @p target.
    void showFailure(config::LaunchTarget const& target, std::string const& reason);

    /// Tells the views the failure under a row changed.
    void notifyFailureRows();

    void setBusy(bool busy);

    ConnectPickerServices _services;
    std::vector<cli::ConnectRow> _rows;
    cli::LocalDaemonState _daemon;
    QString _filter;
    int _current = -1;
    bool _isBusy = false;
    QString _failure;
    QString _failedTarget;
    std::optional<config::LaunchTarget> _lastAttempt; //!< What retry() connects to.
    std::uint64_t _probe = 0;           //!< Counts daemon probes started; a stale answer is dropped.
    std::uint64_t _attempt = 0;         //!< Counts connections started; a stale answer is dropped.
    std::optional<bool> _askOverride;   //!< What the switch shows until a reload carries the stored value.
    std::vector<std::jthread> _workers; //!< The checks started; the last may still run.
};

/// Builds the picker's window on @p engine.
/// @param engine The app's QML engine.
/// @param picker What the window shows; borrowed, it outlives the window.
/// @return The window, not yet shown; null when ConnectPickerWindow.qml did not load (the errors are logged).
[[nodiscard]] std::unique_ptr<QQuickWindow> loadConnectPickerWindow(QQmlEngine& engine,
                                                                    ConnectPickerController& picker);

} // namespace contour::window
