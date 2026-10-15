// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QString>

namespace contour::test
{

/// The window-controller surface Main.qml's own Connections bind, shared by every offscreen mock that
/// loads Main.qml in place of the real WindowController.
///
/// A signal a Connections handler names but the target lacks is a QML warning, which the run-wide gate
/// turns into a failure of the whole suite. Declaring the signals once keeps each mock in step with
/// Main.qml: a handler added there is a line here, not a line in every mock.
class MainWindowControllerMock: public QAbstractListModel
{
    Q_OBJECT

  public:
    using QAbstractListModel::QAbstractListModel;

    /// Mirrors WindowController::trayMessageClicked(), counting the calls.
    Q_INVOKABLE void trayMessageClicked() { ++trayClickCount; }

    /// Mirrors WindowController::requestNotification(): shows the message as the real one does.
    Q_INVOKABLE void requestNotification(QString const& title, QString const& body)
    {
        emit notificationRequested(title, body);
    }

    int trayClickCount = 0; ///< How many tray message clicks Main.qml passed on to the window.

  signals:
    void commandPaletteRequested();
    void contextMenuRequested();
    void titleBarContextMenuRequested();
    void saveLayoutRequested();
    // The window's attention on behalf of a session that may have no pane on screen.
    void notificationRequested(QString const& title, QString const& body);
    void bellRequested(QString const& source, double volume);
    void alertRequested();
};

} // namespace contour::test
