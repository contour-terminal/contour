// vim:syntax=qml
// The "save layout as" name prompt: a small modal popup with a single text field.
//
// Opened by a nameless SaveLayout action — the row the command palette offers, or a key bound to bare
// SaveLayout. Both route through the session manager to THIS window's WindowController, whose
// `saveLayoutRequested` signal main.qml connects to open(). This is the exact mirror of how a colorless
// SetTabColor opens TabColorFlyout: the bare action opens a surface that supplies the argument.
//
// The user types a layout name and presses Enter; the dialog hands it to WindowController::saveLayoutAs,
// which persists the current window's tabs to layouts.yml. A blank name or Escape cancels.
import QtQuick
import QtQuick.Controls

Popup {
    id: root

    // The WindowController. Null-guarded EVERYWHERE below, for the same reason CommandPalette.qml guards
    // its controller: it is destroyed before the QML tree on window close, and QML re-evaluates dependent
    // bindings once more against null during teardown — an unguarded `controller.` would raise a TypeError.
    required property var controller
    // The ApplicationWindow, so closing the prompt can hand keyboard focus back to the terminal.
    required property var window

    modal: true
    focus: true
    // Insets the content (the label + field) from the border. On the Popup rather than the Column, since a
    // Column is a bare positioner with no padding of its own; the Popup resizes its contentItem to the
    // area left inside this padding.
    padding: 12

    // Centered and fixed-width: the prompt holds one short field, so it need not track the window size the
    // way the command palette's list does.
    anchors.centerIn: Overlay.overlay
    width: Math.min(420, root.window ? root.window.width * 0.8 : 420)

    // Live OS palette handle so the popup follows dark/light in realtime (see CommandPalette).
    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    background: Rectangle {
        color: systemPalette.window
        border.color: systemPalette.mid
        border.width: 1
        radius: 6
    }

    // Start empty with the caret already in the field on every open, so the user can type immediately.
    onOpened: {
        nameField.clear();
        nameField.forceActiveFocus();
    }

    // Whatever closed the prompt — Enter, Escape, a click outside — the terminal must get the keyboard
    // back, or the user is left typing into nothing (same law as CommandPalette.qml's onClosed). The save
    // itself has already happened synchronously in accept(); nothing is deferred here because, unlike the
    // palette, this prompt opens no further keyboard-driven surface for a restore to fight over.
    onClosed: {
        if (root.window)
            root.window.restoreTerminalFocus();
    }

    // Saves under the typed name and closes. A blank field is a no-op that keeps the prompt open, so Enter
    // on an empty name never silently dismisses the dialog without saving — the user must type a name or
    // press Escape. WindowController::saveLayoutAs trims and re-checks, so this is a UX guard, not the
    // authority on emptiness.
    function accept() {
        if (nameField.text.trim().length === 0)
            return;
        if (root.controller)
            root.controller.saveLayoutAs(nameField.text);
        root.close();
    }

    contentItem: Column {
        spacing: 6

        Label {
            text: qsTr("Save layout as:")
            color: systemPalette.windowText
        }

        TextField {
            id: nameField
            objectName: "saveLayoutNameField"
            // The Popup sizes this Column to the area inside its padding, so `parent.width` is that inset
            // width — the field spans it without a second margin calculation.
            width: parent.width
            placeholderText: qsTr("Layout name…")

            Keys.onReturnPressed: root.accept()
            Keys.onEnterPressed: root.accept()
            Keys.onEscapePressed: root.close()
        }
    }
}
