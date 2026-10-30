// vim:syntax=qml
// The kit's shortcut field: shows a key chord as the platform spells it (or "None"), and records the next
// chord the user presses.
//
// `chord` is the caller's. Activating the field (a click, Return or Space) starts recording; the next chord
// is asked for through `recorded` with Qt's key code and modifier mask, and recording ends. Escape cancels,
// Delete or Backspace clears it through `cleared`. While recording, every key is the field's, the menu's
// chords too (it accepts the ShortcutOverride), so a chord a menu would take can be tried.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Control {
    id: root
    objectName: "kitShortcutField"

    /// The chord as the platform spells it; empty for none.
    property string chord
    /// What a screen reader calls the field.
    property string name
    /// What a screen reader reads after the name.
    property string description
    /// Whether the user can change it.
    property bool editable: true
    /// Whether the next chord pressed is the one to record.
    property bool recording: false

    /// The user pressed @p key (Qt's key code) with @p modifiers (Qt's keyboard modifier mask).
    signal recorded(int key, int modifiers)
    /// The user asked for no chord.
    signal cleared()

    /// What the field shows now.
    readonly property string _shown: root.recording ? qsTr("Press a shortcut…")
                                                    : (root.chord.length > 0 ? root.chord : qsTr("None"))
    // The keys that are modifiers alone: pressing one is the start of a chord, not a chord.
    readonly property var _modifierKeys: [Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta,
                                          Qt.Key_AltGr, Qt.Key_Super_L, Qt.Key_Super_R]
    // The gaps in the content: before the caption, between it and the button, and after the button.
    readonly property int _paddedGaps: 3

    /// Starts recording.
    function startRecording() {
        if (root.enabled) {
            root.recording = true;
            root.forceActiveFocus();
        }
    }

    /// Stops recording without recording anything.
    function cancelRecording() {
        root.recording = false;
    }

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    activeFocusOnTab: true
    implicitWidth: caption.implicitWidth + button.implicitWidth + root._paddedGaps * chromeStyle.space.s5
    implicitHeight: chromeStyle.settings.control

    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1, %2").arg(root.name).arg(root.chord.length > 0 ? root.chord : qsTr("None"))
    Accessible.description: root.description
    Accessible.focusable: true
    Accessible.onPressAction: root.startRecording()

    // While recording, a chord a menu or a shortcut would take is the field's.
    Keys.onShortcutOverride: event => { event.accepted = root.recording; }
    Keys.onPressed: event => {
        if (!root.recording) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                root.startRecording();
                event.accepted = true;
            }
            return;
        }
        event.accepted = true;
        if (root._modifierKeys.indexOf(event.key) >= 0)
            return;
        if (event.modifiers === Qt.NoModifier && event.key === Qt.Key_Escape) {
            root.cancelRecording();
        } else if (event.modifiers === Qt.NoModifier
                   && (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)) {
            root.cancelRecording();
            root.cleared();
        } else {
            root.cancelRecording();
            root.recorded(event.key, event.modifiers);
        }
    }
    onActiveFocusChanged: if (!root.activeFocus) root.cancelRecording()

    background: Rectangle {
        radius: chromeStyle.radii.control
        color: chromeStyle.colors.controlFill
        border.width: root.visualFocus || root.recording ? chromeStyle.borderWidth : 0
        border.color: chromeStyle.colors.settingsSelectionFill
    }

    contentItem: RowLayout {
        spacing: chromeStyle.space.s5

        Label {
            id: caption
            objectName: "shortcutCaption"
            Layout.fillWidth: true
            Layout.leftMargin: chromeStyle.space.s5
            text: root._shown
            font: chromeStyle.type.body
            color: chromeStyle.colors.controlText
            elide: Text.ElideRight
            textFormat: Text.PlainText
            Accessible.ignored: true
        }

        Button {
            id: button
            objectName: "shortcutRecordButton"
            Layout.rightMargin: chromeStyle.space.s5
            flat: true
            focusPolicy: Qt.NoFocus
            text: root.recording ? qsTr("Cancel") : qsTr("Record")
            font: chromeStyle.type.footnote
            palette.buttonText: chromeStyle.colors.controlText
            Accessible.ignored: true
            onClicked: root.recording ? root.cancelRecording() : root.startRecording()
        }
    }
}
