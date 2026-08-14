// vim:syntax=qml
// One segment of the indicator status line — left, middle or right — as a labelled row of chips with an
// "Add" picker and a raw-template escape hatch.
//
// Purely presentational: it is handed the segment's items and the placeholder catalog and reports what the
// user asked for through signals. The owner (StatusLineIndicatorEditor.qml) does the editing, so the write
// path stays in one place.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property string segmentLabel: ""
    /// The segment's items, as SettingsController::parseIndicatorSegment returned them.
    property var items: []
    /// Every placeholder that may be added, as [{ type, label, sample }].
    property var placeholders: []
    /// The template flag names, passed through to each chip's style badge.
    property var flagKeys: []
    /// The segment's template exactly as the config file holds it.
    property string rawTemplate: ""
    property bool editable: true

    signal addRequested(var entry)
    signal editRequested(int index)
    signal removeRequested(int index)
    signal rawCommitted(string text)

    // Live OS palette handle, so the row follows dark/light in realtime.
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.6)
    readonly property color hairline: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.12)

    /// Whether the raw template field is shown.
    property bool rawVisible: false

    color: "transparent"
    radius: 8
    border.width: 1
    border.color: root.hairline

    // The label sits beside the chip flow, so the row is as tall as the taller of the two -- which is what
    // the inner layout's implicitHeight already is. Adding the label's height to the flow's (as this once
    // did) both over-measured the box and fed the flow's height back through its own width.
    implicitHeight: column.implicitHeight + 16

    ColumnLayout {
        id: column
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            leftMargin: 10
            rightMargin: 10
            topMargin: 8
        }
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: root.segmentLabel
                font.weight: Font.DemiBold
                font.pointSize: 9
                color: root.subtleText
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 5
                Layout.minimumWidth: 46
            }

            Flow {
                Layout.fillWidth: true
                spacing: 4

                Repeater {
                    model: root.items
                    delegate: StatusLineChip {
                        required property var modelData
                        required property int index
                        item: modelData
                        flagKeys: root.flagKeys
                        editable: root.editable
                        onEditRequested: root.editRequested(index)
                        onRemoveRequested: root.removeRequested(index)
                    }
                }

                Button {
                    objectName: "indicatorAddButton"
                    text: qsTr("＋ Add")
                    flat: true
                    font.pointSize: 9
                    enabled: root.editable
                    Accessible.name: qsTr("Add a placeholder to the %1 segment").arg(root.segmentLabel)
                    onClicked: addPopup.open()
                }

                Button {
                    objectName: "indicatorRawToggleButton"
                    text: root.rawVisible ? qsTr("Hide template") : qsTr("Edit as text")
                    flat: true
                    font.pointSize: 8
                    Accessible.name: qsTr("Edit the %1 segment as a raw template").arg(root.segmentLabel)
                    onClicked: root.rawVisible = !root.rawVisible
                }
            }
        }

        // The escape hatch: the template exactly as the config file holds it. The visual editor covers
        // everything the grammar can express today, but a hand-written template is still the way to type
        // something it cannot.
        TextField {
            objectName: "indicatorRawField"
            visible: root.rawVisible
            Layout.fillWidth: true
            Layout.bottomMargin: 4
            enabled: root.editable
            font.family: "monospace"
            font.pointSize: 9
            selectByMouse: true
            text: root.rawTemplate
            // Committed on commit rather than per keystroke, so a half-typed placeholder is not parsed and
            // normalized out from under the cursor.
            onEditingFinished: root.rawCommitted(text)
        }
    }

    // {{{ Add-placeholder picker
    Popup {
        id: addPopup
        y: root.height
        width: Math.min(420, root.width)
        modal: false
        focus: true
        padding: 10
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: PopupSurface {}

        ColumnLayout {
            anchors.fill: parent
            spacing: 6

            Label {
                text: qsTr("Add to the %1 segment").arg(root.segmentLabel)
                font.weight: Font.DemiBold
                color: sys.windowText
            }

            // Every placeholder vtbackend understands, each with the sample it previews as.
            Repeater {
                model: root.placeholders
                delegate: ItemDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    onClicked: {
                        root.addRequested(modelData)
                        addPopup.close()
                    }
                    contentItem: RowLayout {
                        spacing: 8
                        Label {
                            text: modelData.label
                            color: sys.windowText
                            font.pointSize: 9
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: modelData.sample
                            color: root.subtleText
                            font.family: "monospace"
                            font.pointSize: 8
                            elide: Text.ElideRight
                            Layout.maximumWidth: 150
                        }
                    }
                }
            }
        }
    }
    // }}}
}
