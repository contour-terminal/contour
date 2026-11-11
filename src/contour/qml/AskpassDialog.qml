// vim:syntax=qml
// The dialog of an ssh login: what ssh asks through the askpass helper (PromptHost.pendingPrompt), as a
// KitDialog -- a masked field for a password or passphrase, a Yes/No pair for a question, a Cancel for a
// notice that something outside the window (a security key's touch) is awaited.
//
// A question that compares things -- a host key's fingerprint -- shows ssh's own text verbatim, in a
// selectable monospaced block, and its default button is No. The secret field never reaches an accessible
// description, and is cleared the moment the dialog closes, answered or not.
//
// It answers through `answered`, exactly once per prompt shown: a button, Enter, or Escape (which is Cancel).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

KitDialog {
    id: root
    objectName: "askpassDialog"

    /// The prompt, as PromptHost.pendingPrompt publishes it; `({})` while none is asked.
    required property var prompt
    /// Where the focus goes once the dialog closes: the terminal it asked over.
    property var focusTarget: null

    /// The user answered: the prompt's id, an AskpassOutcome as its number and the answer (the typed secret
    /// for a password, the button's text otherwise).
    signal answered(int promptId, int outcome, string answer)

    // Set as each prompt opens, so a doubled close (a button, then a stray Escape) answers only once.
    property bool _answered: false

    // The most the verbatim block may take: half of what the dialog hangs in.
    readonly property real _blockRoom: root.parent ? root.parent.height / 2 : verbatimText.implicitHeight

    // A notice has no accept button: its one button, Cancel, takes the accept place.
    readonly property bool _hasAccept: root.prompt.acceptText !== undefined && root.prompt.acceptText !== ""

    function _answer(outcome, answer) {
        if (root._answered || root.prompt.promptId === undefined)
            return;
        root._answered = true;
        root.answering = true;
        root.answered(root.prompt.promptId, outcome, answer);
        root.answering = false;
        secretField.text = "";
    }

    function _accept() {
        if (!root._hasAccept)
            root._reject();
        else
            root._answer(root.prompt.acceptOutcome, root.prompt.isSecret ? secretField.text : root.prompt.acceptAnswer);
    }

    function _reject() {
        root._answer(root.prompt.rejectOutcome, root.prompt.rejectAnswer);
    }

    /// Dismisses the prompt shown, as Cancel does; nothing when none is.
    function cancel() {
        root._reject();
    }

    heading: root.prompt.heading !== undefined ? root.prompt.heading : ""
    // A verbatim prompt's text is the block's; the others say it here, in the dialog's message.
    message: root.prompt.isVerbatim === true || root.prompt.text === undefined ? "" : root.prompt.text
    // What a screen reader reads: ssh's own words. The field's content is never among them.
    description: root.prompt.text !== undefined ? root.prompt.text : ""
    acceptText: root._hasAccept ? root.prompt.acceptText : (root.prompt.rejectText ?? "")
    rejectText: root._hasAccept ? (root.prompt.rejectText ?? "") : ""
    // A question about trust or identity is answered No unless the user says otherwise.
    defaultButton: root.prompt.isVerbatim === true ? KitDialog.Reject : KitDialog.Accept
    // Escape is no answer to keep: it is Cancel, below.
    closePolicy: Popup.NoAutoClose

    // True while `answered` is being emitted. The dialog closes only after that returns, so a prompt put up from
    // inside the answer (the next one) must be presented once it has: presented now, the close would take it too.
    property bool answering: false
    // The latest prompt present() was given: what the deferred presentation shows, never a stale one.
    property var _wanted: ({})

    /// Shows @p question: opens the dialog, or, while it is open, shows what is asked now. A prompt other than
    /// the one shown starts afresh -- unanswered, the field empty. An empty @p question closes the dialog.
    function present(question) {
        root._wanted = question;
        if (root.answering)
            Qt.callLater(root._presentWanted);
        else
            root._presentWanted();
    }

    function _presentWanted() {
        if (root.answering)
            return;
        let question = root._wanted;
        if (question.promptId === undefined) {
            // Taken away (withdrawn, or answered): no answer is due.
            root._answered = true;
            secretField.text = "";
            if (root.visible)
                root.close();
            return;
        }
        let fresh = !root.visible || question.promptId !== root.prompt.promptId;
        root.prompt = question;
        if (fresh) {
            root._answered = false;
            secretField.text = "";
        }
        if (fresh || !root.visible)
            root.open();
    }

    onAccepted: root._accept()
    onRejected: root._reject()
    onEscaped: {
        root._reject();
        root.close();
    }
    onOpened: if (root.prompt.isSecret === true) secretField.forceActiveFocus()
    onClosed: {
        secretField.text = "";
        if (root.focusTarget !== null && root.focusTarget !== undefined)
            root.focusTarget.forceActiveFocus();
    }

    bodyData: [
        TextField {
            id: secretField
            objectName: "askpassSecret"
            Layout.fillWidth: true
            visible: root.prompt.isSecret === true
            echoMode: TextInput.Password
            font: chromeStyle.type.footnote
            color: sys.windowText
            // A password: no prediction, no history, no suggestions.
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Password or passphrase")
            onAccepted: root.accept()
        },
        ScrollView {
            id: verbatimScroll
            objectName: "askpassVerbatimScroll"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(verbatimText.implicitHeight, root._blockRoom)
            visible: root.prompt.isVerbatim === true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            TextArea {
                id: verbatimText
                objectName: "askpassVerbatim"
                width: verbatimScroll.availableWidth
                readOnly: true
                selectByMouse: true
                text: root.prompt.text !== undefined ? root.prompt.text : ""
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
                font: Qt.font({ family: "monospace", pixelSize: chromeStyle.type.footnote.pixelSize })
                color: sys.windowText
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("What ssh says")
            }
        }
    ]

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
}
