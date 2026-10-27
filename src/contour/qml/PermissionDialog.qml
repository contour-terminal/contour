// vim:syntax=qml
// The permission wall's one dialog: what a session asks its user (TerminalSession.pendingPermission), as a
// KitDialog -- titled by the action, its detail as lines, one checkbox for how long the answer stands, and
// the two answers, Deny and Allow, labelled by the question.
//
// It answers through `answered`, exactly once per question shown: a button, or Escape, which is Deny for
// this once whatever the box says. Every way it closes hands the focus back to `focusTarget`.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Contour.Terminal

KitDialog {
    id: root
    objectName: "permissionDialog"

    /// The question, as TerminalSession.pendingPermission() publishes it; `({})` while none is asked.
    required property var prompt
    /// Where the focus goes once the dialog closes: the terminal it asked over.
    property var focusTarget: null

    /// The user answered: a Permission.PermissionDecision and a Permission.PermissionMemory.
    signal answered(int decision, int memory)

    // Set as each question opens, so a doubled close (a button, then a stray Escape) answers only once.
    property bool _answered: false

    readonly property var _lines: root.prompt.lines !== undefined ? root.prompt.lines : []
    // The most the lines may take: half of what the dialog hangs in.
    readonly property real _linesRoom: root.parent ? root.parent.height / 2 : linesColumn.implicitHeight

    function _answer(decision, memory) {
        if (root._answered)
            return;
        root._answered = true;
        root.answered(decision, memory);
    }

    function _memoryFor(checked) {
        return checked ? root.prompt.rememberScope : Permission.Once;
    }

    heading: root.prompt.title !== undefined ? root.prompt.title : ""
    message: ""
    description: root._lines.map(line => line.text).join("\n")
    acceptText: root.prompt.allowText !== undefined ? root.prompt.allowText : ""
    rejectText: root.prompt.denyText !== undefined ? root.prompt.denyText : ""
    acceptEnabled: !(root.prompt.allowOnce === Permission.Withheld && !permissionRemember.checked)
    defaultButton: root.prompt.defaultDecision === Permission.Allow ? KitDialog.Accept : KitDialog.Reject
    // Escape is no answer to keep: it is answered below, as Deny for this once (never the box's scope).
    closePolicy: Popup.NoAutoClose

    /// Shows @p question: opens the dialog, or, while it is open, shows what the question asks now. A question
    /// other than the one shown starts afresh -- unanswered, the box as the question sets it.
    function present(question) {
        let fresh = !root.visible || question.id !== root.prompt.id;
        root.prompt = question;
        if (fresh) {
            root._answered = false;
            permissionRemember.checked = question.initialMemory !== undefined
                                         && question.initialMemory !== Permission.Once;
        }
        // Opened again even while it closes: a question re-presented is closed and opened in one step.
        if (fresh || !root.visible)
            root.open();
    }
    onAccepted: root._answer(Permission.Allow, root._memoryFor(permissionRemember.checked))
    onRejected: root._answer(Permission.Deny, root._memoryFor(permissionRemember.checked))
    onEscaped: {
        root._answer(Permission.Deny, Permission.Once);
        root.close();
    }
    onClosed: if (root.focusTarget !== null && root.focusTarget !== undefined) root.focusTarget.forceActiveFocus()

    bodyData: [
        // The detail scrolls once it is taller than half the window -- an extension may declare many
        // commands -- so the checkbox and the two buttons always stay in view.
        ScrollView {
            id: permissionLines
            objectName: "permissionLines"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(linesColumn.implicitHeight, root._linesRoom)
            clip: true
            contentWidth: availableWidth
            contentHeight: linesColumn.implicitHeight

            ColumnLayout {
                id: linesColumn
                width: permissionLines.availableWidth
                spacing: chromeStyle.space.s3

                Repeater {
                    model: root._lines

                    Label {
                        required property var modelData
                        objectName: "permissionLine"
                        Layout.fillWidth: true
                        text: modelData.text
                        // Program-supplied, made safe by the session: text, never markup.
                        textFormat: Text.PlainText
                        font: modelData.weight === Permission.Strong ? chromeStyle.type.footnoteStrong
                                                                     : chromeStyle.type.footnote
                        color: sys.windowText
                        wrapMode: modelData.style === Permission.Verbatim ? Text.NoWrap : Text.Wrap
                        elide: modelData.style === Permission.Verbatim ? Text.ElideRight : Text.ElideNone
                    }
                }
            }
        },
        CheckBox {
            id: permissionRemember
            objectName: "permissionRemember"
            text: root.prompt.rememberText !== undefined ? root.prompt.rememberText : ""
            font: chromeStyle.type.footnote
            palette.windowText: sys.windowText
            Accessible.role: Accessible.CheckBox
            Accessible.name: text
        }
    ]

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
}
