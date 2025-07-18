// vim:syntax=qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Qt.labs.platform

ApplicationWindow
{
    id: appWindow
    visible: true

    // Application window's background must be transparent in order to support transparent/semi-transparent
    // background in the terminal widgets.
    // color: "transparent"
    color: Qt.rgba(0, 0, 0, 0.0)

    // NOTE: Can't be done because we cannot set vtui.session upon initialization due to a Qt5 bug
    // that would keep recreating & re-assigning new sessions every time a new window is created
    // for ALL already existing windows again and again and again, ...
    //
    // That is why the Component.onCompleted workaround is used until we require Qt6. Sad.
    //
    // title: "%1 - Contour".arg(vtui.session.title)
    title: vtui.title

    width: vtui.implicitWidth
    height: vtui.implicitHeight

    Terminal {
        id: vtui
        focus: true
        visible : true
        anchors.fill: parent
        onShowNotification: (title, content) => appWindow.showNotification(title, content)
        onOpacityChanged: appWindow.applyOpacity()
    }

    onClosing: {
        console.log("Terminal closed. Removing session.");
        terminalSessions.closeWindow();
    }


    onWidthChanged : function() {
        vtui.width = width
        vtui.updateSizeWidget()
    }

    onHeightChanged: function() {
        vtui.height = height
        vtui.updateSizeWidget()
    }

    function applyOpacity() {
        vtui.opacity = vtui.session.opacity;
    }

    function showNotification(title, content) {
        // "OSC 777 ; notify ; <TITLE> ; <CONTENT> ST"
        // Example: printf "\033]777;notify;Hello Title;Hello Content\033\\"
        console.log("main: notification [%1] %2".arg(title).arg(content));
        if (trayIcon.supportsMessages)
        {
            trayIcon.show();
            trayIcon.showMessage("Application Message: %1".arg(title),
                                 "%1".arg(content),
                                 60 * 1000);
        }
        else
            console.log("main: Notification system not supported!");
    }

    // NB: This requires Qt 5.12+
    // See https://doc.qt.io/qt-5/qml-qt-labs-platform-systemtrayicon.html#availability for details.
    SystemTrayIcon {
        id: trayIcon
        visible: false
        icon.source: "qrc:/contour/logo-256.png"
        icon.name: "Contour Terminal"

        menu: Menu {
            MenuItem {
                text: qsTr("Quit")
                onTriggered: Qt.quit()
            }
        }

        onMessageClicked: hide()
    }
}
