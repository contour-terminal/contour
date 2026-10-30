// vim:syntax=qml
// The Connect picker in a window of its own: a small window shown before any terminal window, or, as the
// sheet of File > New Window to…, the same window made modal over the terminal window that asked.
//
// Built by the app (ContourGuiApp::openConnectPicker) over a ConnectPickerController, which decides what the
// rows are and what choosing one does. The window only sizes itself from the chrome tokens and closes when
// the app closes it once the controller says the job is done or given up.
import QtQuick
import QtQuick.Controls

ThemedWindow
{
    id: pickerWindow
    objectName: "connectPickerWindow"

    // The ConnectPickerController.
    required property var picker

    title: qsTr("Connect")
    visible: false
    width: chromeStyle.size.connectWidth
    height: chromeStyle.size.connectHeight
    minimumWidth: chromeStyle.size.connectWidth
    minimumHeight: chromeStyle.size.connectHeight

    ConnectPicker
    {
        id: content
        anchors.fill: parent
        picker: pickerWindow.picker
    }

    // Closed by the window manager: a check still running must not open a window afterwards.
    onClosing: if (pickerWindow.picker) pickerWindow.picker.abandon()

    // Focus goes to the field whenever the window is shown again.
    onVisibleChanged: if (visible) content.focusField()
}
