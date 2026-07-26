// vim:syntax=qml
// The style sheet of one indicator status line placeholder: every cell flag the template grammar accepts,
// both colours, the side adornments, and whatever else that particular placeholder carries — plus the
// reorder and remove actions for its position in its segment.
//
// Call edit() with the item to work on and it opens; it reports the result through applied(), removed() or
// moved() and never touches the controller itself, so the whole write path stays in
// StatusLineIndicatorEditor.
//
// The edited item is held as individual properties rather than as one JS object, because a plain object's
// fields emit no change notification: bound directly to an object's fields, the colour swatches and flag
// buttons silently stop tracking what the user types.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    /// Every cell flag an item may carry, as [{ key, label }], from SettingsController::indicatorFlags.
    property var flagCatalog: []

    /// Emitted with the edited item when the user accepts.
    signal applied(int segmentIndex, int itemIndex, var item)
    /// Emitted when the user removes the item outright.
    signal removed(int segmentIndex, int itemIndex)
    /// Emitted when the user moves the item by @p delta places within its segment.
    signal moved(int segmentIndex, int itemIndex, int delta)

    // Live OS palette handle, so the dialog follows dark/light in realtime.
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.6)
    readonly property color hairline: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.12)

    // Which item is being edited.
    property int segmentIndex: -1
    property int itemIndex: -1
    property int segmentLength: 0

    // The edited item, field by field. See the note at the top for why these are not one object.
    property string itemType: ""
    property string itemLabel: ""
    property string itemText: ""
    property string itemCommand: ""
    property bool hasColor: false
    property string colorHex: ""
    property bool hasBgColor: false
    property string bgColorHex: ""
    property string textLeft: ""
    property string textRight: ""
    property var flags: ({})
    property bool hasActiveColor: false
    property string activeColorHex: ""
    property bool hasActiveBackground: false
    property string activeBackgroundHex: ""
    property string separator: ""

    readonly property bool isText: root.itemType === "Text"
    readonly property bool isCommand: root.itemType === "Command"
    readonly property bool isTabs: root.itemType === "Tabs"

    /// A template has no escaping, so these characters cannot survive inside an attribute value. Rejecting
    /// them here is what keeps serializeStatusLineSegment from having to drop the styling in order to keep
    /// the text — which is what it does when it sees one.
    readonly property bool textIsRepresentable: root.itemText.indexOf(",") < 0
                                                && root.itemText.indexOf("{") < 0
                                                && root.itemText.indexOf("}") < 0

    function setFlag(key, value) {
        // Reassigned wholesale: mutating the existing object in place notifies nothing.
        var next = {}
        for (var k in root.flags)
            next[k] = root.flags[k]
        next[key] = value
        root.flags = next
    }

    /// Loads @p item into the editable properties above, then opens.
    function edit(segmentIndex, itemIndex, segmentLength, item) {
        if (!item)
            return
        root.segmentIndex = segmentIndex
        root.itemIndex = itemIndex
        root.segmentLength = segmentLength
        root.itemType = item.type !== undefined ? item.type : ""
        root.itemLabel = item.label !== undefined ? item.label : root.itemType
        root.itemText = item.text !== undefined ? item.text : ""
        root.itemCommand = item.command !== undefined ? item.command : ""
        root.hasColor = item.hasColor === true
        root.colorHex = item.color !== undefined ? item.color : ""
        root.hasBgColor = item.hasBgColor === true
        root.bgColorHex = item.bgColor !== undefined ? item.bgColor : ""
        root.textLeft = item.textLeft !== undefined ? item.textLeft : ""
        root.textRight = item.textRight !== undefined ? item.textRight : ""
        root.hasActiveColor = item.hasActiveColor === true
        root.activeColorHex = item.activeColor !== undefined ? item.activeColor : ""
        root.hasActiveBackground = item.hasActiveBackground === true
        root.activeBackgroundHex = item.activeBackground !== undefined ? item.activeBackground : ""
        root.separator = item.separator !== undefined ? item.separator : ""

        var flags = {}
        var source = item.flags !== undefined ? item.flags : ({})
        for (var i = 0; i < root.flagCatalog.length; ++i) {
            var key = root.flagCatalog[i].key
            flags[key] = source[key] === true
        }
        root.flags = flags

        root.open()
    }

    /// Assembles the edited properties back into an item and reports it.
    function apply() {
        var item = {
            "type": root.itemType,
            "label": root.itemLabel,
            "displayName": root.itemLabel,
            "flags": root.flags,
            "hasColor": root.hasColor,
            "color": root.colorHex,
            "hasBgColor": root.hasBgColor,
            "bgColor": root.bgColorHex,
            "textLeft": root.textLeft,
            "textRight": root.textRight
        }
        // A Text or Command item is named by its own content, the way parseIndicatorSegment names it.
        if (root.isText) {
            item.text = root.itemText
            item.displayName = root.itemText.length > 0 ? root.itemText : root.itemLabel
        }
        if (root.isCommand) {
            item.command = root.itemCommand
            item.displayName = root.itemCommand.length > 0 ? root.itemCommand : root.itemLabel
        }
        if (root.isTabs) {
            item.hasActiveColor = root.hasActiveColor
            item.activeColor = root.activeColorHex
            item.hasActiveBackground = root.hasActiveBackground
            item.activeBackground = root.activeBackgroundHex
            item.separator = root.separator
        }
        root.applied(root.segmentIndex, root.itemIndex, item)
        root.close()
    }

    // Parented to the window overlay rather than to the editor, so it is centred on the window and cannot
    // be clipped by the scrolling settings pane it was opened from.
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(480, Overlay.overlay ? Overlay.overlay.width - 48 : 480)
    modal: true
    focus: true
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: root.itemLabel
                font.pointSize: 12
                font.weight: Font.DemiBold
                color: sys.windowText
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: "{" + root.itemType + "}"
                color: root.subtleText
                font.family: "monospace"
                font.pointSize: 9
            }
        }

        // Text content, for the placeholders that carry some.
        ColumnLayout {
            visible: root.isText || root.isCommand
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: root.isCommand ? qsTr("Program") : qsTr("Text")
                    color: sys.windowText
                    Layout.preferredWidth: 130
                }
                TextField {
                    objectName: "indicatorItemTextField"
                    Layout.fillWidth: true
                    text: root.isCommand ? root.itemCommand : root.itemText
                    selectByMouse: true
                    onTextChanged: {
                        if (root.isCommand)
                            root.itemCommand = text
                        else
                            root.itemText = text
                    }
                }
            }
            Label {
                objectName: "indicatorTextWarning"
                visible: root.isText && !root.textIsRepresentable
                Layout.fillWidth: true
                Layout.leftMargin: 138
                wrapMode: Text.WordWrap
                font.pointSize: 8
                // Not merely advisory: with one of these in it, the text can only be stored unstyled,
                // because a template cannot escape them.
                text: qsTr("A comma or a brace cannot be stored with styling — remove it, or the styling "
                           + "on this text will be dropped.")
                color: "#e0a030"
            }
        }

        // Side adornments.
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 8
            rowSpacing: 6

            Label {
                text: qsTr("Before")
                color: sys.windowText
                Layout.preferredWidth: 130
            }
            TextField {
                Layout.fillWidth: true
                text: root.textLeft
                placeholderText: qsTr("e.g. “ │ ”")
                selectByMouse: true
                onTextChanged: root.textLeft = text
            }

            Label {
                text: qsTr("After")
                color: sys.windowText
                Layout.preferredWidth: 130
            }
            TextField {
                Layout.fillWidth: true
                text: root.textRight
                selectByMouse: true
                onTextChanged: root.textRight = text
            }
        }

        // Every flag the template grammar accepts, straight from C++.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Label {
                text: qsTr("Style")
                color: sys.windowText
                font.weight: Font.DemiBold
            }
            Flow {
                Layout.fillWidth: true
                spacing: 4
                Repeater {
                    model: root.flagCatalog
                    delegate: Button {
                        required property var modelData
                        objectName: "indicatorFlagButton"
                        text: modelData.label
                        font.pointSize: 8
                        flat: true
                        checkable: true
                        checked: root.flags[modelData.key] === true
                        Accessible.name: modelData.label
                        onToggled: root.setFlag(modelData.key, checked)
                    }
                }
            }
        }

        // Colours. Each is a hex field plus a swatch; clearing the field unsets the colour.
        ColorSlotField {
            Layout.fillWidth: true
            label: qsTr("Foreground")
            isSet: root.hasColor
            value: root.colorHex
            onChanged: (isSet, hex) => { root.hasColor = isSet; root.colorHex = hex }
        }
        ColorSlotField {
            Layout.fillWidth: true
            label: qsTr("Background")
            isSet: root.hasBgColor
            value: root.bgColorHex
            onChanged: (isSet, hex) => { root.hasBgColor = isSet; root.bgColorHex = hex }
        }
        ColorSlotField {
            Layout.fillWidth: true
            visible: root.isTabs
            label: qsTr("Active tab foreground")
            isSet: root.hasActiveColor
            value: root.activeColorHex
            onChanged: (isSet, hex) => { root.hasActiveColor = isSet; root.activeColorHex = hex }
        }
        ColorSlotField {
            Layout.fillWidth: true
            visible: root.isTabs
            label: qsTr("Active tab background")
            isSet: root.hasActiveBackground
            value: root.activeBackgroundHex
            onChanged: (isSet, hex) => { root.hasActiveBackground = isSet; root.activeBackgroundHex = hex }
        }

        // Tabs' own separator.
        RowLayout {
            visible: root.isTabs
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("Tab separator")
                color: sys.windowText
                Layout.preferredWidth: 130
            }
            TextField {
                Layout.fillWidth: true
                text: root.separator
                placeholderText: qsTr("default: |")
                selectByMouse: true
                onTextChanged: root.separator = text
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.hairline }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // Reordering lives here rather than on the chip: a chip with arrows on it is three hit targets
            // in twenty pixels, and this is where the user already is.
            Button {
                objectName: "indicatorMoveEarlierButton"
                text: "←"
                flat: true
                enabled: root.itemIndex > 0
                Accessible.name: qsTr("Move earlier")
                onClicked: {
                    root.moved(root.segmentIndex, root.itemIndex, -1)
                    root.close()
                }
            }
            Button {
                objectName: "indicatorMoveLaterButton"
                text: "→"
                flat: true
                enabled: root.itemIndex >= 0 && root.itemIndex < root.segmentLength - 1
                Accessible.name: qsTr("Move later")
                onClicked: {
                    root.moved(root.segmentIndex, root.itemIndex, 1)
                    root.close()
                }
            }
            Button {
                objectName: "indicatorRemoveItemButton"
                text: qsTr("Remove")
                flat: true
                onClicked: {
                    root.removed(root.segmentIndex, root.itemIndex)
                    root.close()
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            Button {
                objectName: "indicatorApplyButton"
                text: qsTr("Apply")
                highlighted: true
                onClicked: root.apply()
            }
        }
    }
}
