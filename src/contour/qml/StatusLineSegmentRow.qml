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
    /// The width of the label's column: the owner sets the widest of its rows' labels, so the chips line up.
    /// Never narrower than this row's own label.
    property real labelWidth: 0
    /// The width this row's label needs on its own.
    readonly property real naturalLabelWidth: segmentTitle.implicitWidth
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

    /// Whether the raw template field is shown.
    property bool rawVisible: false

    color: "transparent"
    radius: chromeStyle.radii.control

    // The label sits beside the chip flow, so the row is as tall as the taller of the two -- which is what
    // the inner layout's implicitHeight already is. Adding the label's height to the flow's (as this once
    // did) both over-measured the box and fed the flow's height back through its own width.
    implicitHeight: column.implicitHeight + chromeStyle.space.s4 + chromeStyle.space.s4

    ColumnLayout {
        id: column
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            leftMargin: chromeStyle.settings.rowPadding
            rightMargin: chromeStyle.settings.rowPadding
            topMargin: chromeStyle.space.s4
        }
        spacing: chromeStyle.space.s3

        RowLayout {
            Layout.fillWidth: true
            spacing: chromeStyle.space.s5

            Label {
                id: segmentTitle
                objectName: "indicatorSegmentLabel"
                text: root.segmentLabel
                font: chromeStyle.type.footnoteStrong
                color: chromeStyle.colors.settingsHelp
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: chromeStyle.space.s3
                Layout.preferredWidth: Math.max(root.labelWidth, root.naturalLabelWidth, chromeStyle.settings.control)
            }

            Flow {
                Layout.fillWidth: true
                spacing: chromeStyle.space.s2

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
                    font: chromeStyle.type.footnote
                    enabled: root.editable
                    Accessible.name: qsTr("Add a placeholder to the %1 segment").arg(root.segmentLabel)
                    onClicked: addPopup.open()
                }

                Button {
                    objectName: "indicatorRawToggleButton"
                    text: root.rawVisible ? qsTr("Hide template") : qsTr("Edit as text")
                    flat: true
                    font: chromeStyle.type.footnote
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
            Layout.bottomMargin: chromeStyle.space.s2
            enabled: root.editable
            font: Qt.font({ family: "monospace", pixelSize: chromeStyle.type.body.pixelSize })
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
        width: Math.min(chromeStyle.settings.listMaxWidth, root.width)
        modal: false
        focus: true
        padding: chromeStyle.space.s5
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: PopupSurface {}

        ColumnLayout {
            anchors.fill: parent
            spacing: chromeStyle.space.s3

            Label {
                text: qsTr("Add to the %1 segment").arg(root.segmentLabel)
                font: chromeStyle.type.bodyStrong
                color: chromeStyle.colors.settingsLabel
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
                        spacing: chromeStyle.space.s5
                        Label {
                            text: modelData.label
                            color: chromeStyle.colors.settingsLabel
                            font: chromeStyle.type.footnote
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: modelData.sample
                            color: chromeStyle.colors.settingsHelp
                            font: Qt.font({ family: "monospace", pixelSize: chromeStyle.type.footnote.pixelSize })
                            elide: Text.ElideRight
                            Layout.maximumWidth: chromeStyle.settings.sidebarWidth
                        }
                    }
                }
            }
        }
    }
    // }}}
}
