// vim:syntax=qml
// Save Layout: the kit dialog a nameless SaveLayout action opens -- the row the command palette offers, or a
// key bound to bare SaveLayout. Both route through the session manager to THIS window's WindowController,
// whose `saveLayoutRequested` signal Main.qml connects to open().
//
// It says what it saves (this window's tabs and panes, with their working directories and commands), takes a
// name, and keeps Save disabled while the name is empty. A name a layout already has offers "Replace?"
// inline; once confirmed, the accept button replaces it, painted as the destructive action it is. Typing
// again withdraws the confirmation. The window's controller checks the name and saves; a sheet on macOS.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

KitDialog {
    id: root
    objectName: "saveLayoutDialog"

    // The WindowController. Null-guarded EVERYWHERE below, for the same reason CommandPalette.qml guards
    // its controller: it is destroyed before the QML tree on window close, and QML re-evaluates dependent
    // bindings once more against null during teardown -- an unguarded `controller.` would raise a TypeError.
    required property var controller
    // The ApplicationWindow, so closing the dialog can hand keyboard focus back to the terminal.
    required property var window

    // Whether the user confirmed replacing the layout the typed name already names.
    property bool confirmedReplace: false

    // What the controller makes of the typed name. `typeof` for the reason the hosting notice guards with
    // `||`: a lightweight mock `win` (the routing tests') lacks the save-layout surface.
    readonly property bool _canCheck: !!root.controller && typeof root.controller.checkLayoutName === "function"
    readonly property var check: root._canCheck
                                 ? root.controller.checkLayoutName(nameField.text, root.confirmedReplace)
                                 : ({ "nameState": "empty", "button": "disabled", "trimmed": "" })
    readonly property bool _replacing: root.check.button === "replace"

    heading: qsTr("Save Layout")
    message: ""
    acceptText: root._replacing ? qsTr("Replace") : qsTr("Save")
    acceptRole: root._replacing ? chromeStyle.statusDanger : chromeStyle.statusRunning
    acceptEnabled: root.check.button !== "disabled"
    rejectText: qsTr("Cancel")

    // Start empty with the caret already in the field on every open, so the user can type immediately.
    onOpened: {
        nameField.clear();
        root.confirmedReplace = false;
        nameField.forceActiveFocus();
    }

    onAccepted: {
        if (!root.controller)
            return;
        if (root._replacing)
            root.controller.replaceLayout(root.check.trimmed);
        else
            root.controller.saveLayoutAs(root.check.trimmed);
    }

    // Whatever closed the dialog -- Save, Cancel, Escape -- the terminal must get the keyboard back, or the
    // user is left typing into nothing (same law as CommandPalette.qml's onClosed).
    onClosed: {
        if (root.window)
            root.window.restoreTerminalFocus();
    }

    bodyData: [
        Label {
            Layout.fillWidth: true
            text: qsTr("Name")
            font: chromeStyle.type.footnote
            color: sys.windowText
        },
        TextField {
            id: nameField
            objectName: "saveLayoutNameField"
            Layout.fillWidth: true
            placeholderText: qsTr("Layout name…")
            Accessible.name: qsTr("Layout name")
            // Another name is another question: a confirmation was for the name it confirmed.
            onTextChanged: root.confirmedReplace = false
            // Return saves only when Save can: the dialog's default button, never a disabled one.
            Keys.onReturnPressed: root.pressDefault()
            Keys.onEnterPressed: root.pressDefault()
        },
        Label {
            objectName: "saveLayoutSummary"
            Layout.fillWidth: true
            text: root._canCheck ? root.controller.layoutSummary() : ""
            textFormat: Text.PlainText
            font: chromeStyle.type.footnote
            color: sys.windowText
            wrapMode: Text.Wrap
        },
        RowLayout {
            objectName: "saveLayoutTaken"
            Layout.fillWidth: true
            visible: root.check.nameState === "taken" && !root.confirmedReplace
            spacing: chromeStyle.space.s4

            Label {
                Layout.fillWidth: true
                text: qsTr("“%1” already exists.").arg(root.check.trimmed)
                // A name the user typed: text, never markup.
                textFormat: Text.PlainText
                font: chromeStyle.type.footnote
                color: sys.windowText
                elide: Text.ElideRight
            }

            Button {
                objectName: "saveLayoutReplace"
                flat: true
                text: qsTr("Replace?")
                font: chromeStyle.type.footnote
                palette.buttonText: sys.buttonText
                Accessible.name: text
                onClicked: root.confirmedReplace = true
            }
        }
    ]

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
}
