// vim:syntax=qml
import Qt.labs.platform

MessageDialog {
    id: messageDialog
    // icon: StandardIcon.Question
    // TODO: which permissions exactly? Fill me in!
    title: qsTr("Host application is requesting permissions")
    text: qsTr("The host application is requesting special permissions.")

    buttons: MessageDialog.Yes | MessageDialog.YesToAll | MessageDialog.No | MessageDialog.NoToAll | MessageDialog.Abort

    function clickedRememberChoice() {
        return messageDialog.clickedButton == MessageDialog.YesToAll
            || messageDialog.clickedButton == MessageDialog.NoToAll;
    }
}
