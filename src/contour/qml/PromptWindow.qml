// vim:syntax=qml
// The window that shows an ssh login's prompts while no terminal window exists to show them: `contour client
// --host ssh://...` attaches before its first window opens, and the attach cannot finish before the user has
// answered. It is shown only while a prompt is open, and carries nothing but the prompt's dialog; once a
// terminal window exists, new prompts go there and this one stays hidden.
import QtQuick
import QtQuick.Controls

ThemedWindow {
    id: root
    objectName: "promptWindow"

    /// The window::PromptHost whose prompts this window shows.
    required property var host

    title: qsTr("Contour")
    // Room for the dialog and, for a host key, for the text it quotes: twice the dialog's own width, either way.
    width: chromeStyle.size.dialogWidth + chromeStyle.size.dialogWidth
    height: chromeStyle.size.dialogWidth + chromeStyle.size.dialogWidth
    flags: Qt.Dialog
    visible: root.host.pendingPrompt.promptId !== undefined

    // The window's own close button dismisses the prompt shown: Cancel.
    onClosing: askpassDialog.cancel()

    AskpassDialog {
        id: askpassDialog
        prompt: ({})
        onAnswered: (promptId, outcome, answer) => root.host.answerPrompt(promptId, outcome, answer)
        Component.onCompleted: askpassDialog.present(root.host.pendingPrompt)
    }

    Connections {
        target: root.host
        function onPendingPromptChanged() { askpassDialog.present(root.host.pendingPrompt) }
    }
}
