// vim:syntax=qml
// The kit's pop-up button: shows the chosen option's label and opens a list of the others beneath it.
//
// `value` is the caller's; choosing a row asks for it through `picked` and closes the list. With
// `searchable` the list has a search field above it that keeps the rows whose label contains the text
// (case-insensitively). Up and Down move through the rows, Return chooses, Escape closes.
//
// Not editable, it is disabled: dimmed to `settings.disabledOpacity`, and it takes no input.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Control {
    id: root
    objectName: "kitPopUp"

    /// The options' values, in order.
    property var options: []
    /// The options' labels, same length as `options`; empty shows the values.
    property var labels: []
    /// The chosen option's value.
    property string value
    /// What a screen reader calls the pop-up.
    property string name
    /// What a screen reader reads after the name.
    property string description
    /// Whether the user can change it.
    property bool editable: true
    /// Whether the list has a search field.
    property bool searchable: false
    /// How many rows the list shows before it scrolls.
    property int maxRows: 8

    /// The user asked for the option @p value.
    signal picked(string value)

    /// @return The label shown for the option at @p index.
    function labelAt(index) {
        return root.labels.length > index ? root.labels[index] : root.options[index];
    }

    /// The rows that match the search text: { value, label } each, in the options' order.
    readonly property var _rows: {
        const needle = search.text.toLowerCase();
        const rows = [];
        for (let index = 0; index < root.options.length; ++index) {
            const label = root.labelAt(index);
            if (needle.length === 0 || label.toLowerCase().indexOf(needle) >= 0)
                rows.push({ value: root.options[index], label: label });
        }
        return rows;
    }
    readonly property int _chosenIndex: root.options.indexOf(root.value)
    // The button's content is padded at its start, between the caption and the chevron, and at its end.
    readonly property int _paddedGaps: 3

    /// Opens the list.
    function open() {
        if (root.enabled)
            popup.open();
    }

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    activeFocusOnTab: true
    implicitWidth: caption.implicitWidth + chevron.implicitWidth + root._paddedGaps * chromeStyle.space.s5
    implicitHeight: chromeStyle.settings.control

    Accessible.role: Accessible.ComboBox
    Accessible.name: root.name
    Accessible.description: root.description
    Accessible.focusable: true
    Accessible.onPressAction: root.open()

    Keys.onSpacePressed: root.open()
    Keys.onReturnPressed: root.open()
    Keys.onDownPressed: root.open()

    TapHandler {
        onTapped: {
            root.forceActiveFocus();
            root.open();
        }
    }

    background: Rectangle {
        radius: chromeStyle.radii.control
        color: chromeStyle.colors.controlFill
        border.width: root.visualFocus ? chromeStyle.borderWidth : 0
        border.color: chromeStyle.colors.settingsSelectionFill
    }

    contentItem: RowLayout {
        spacing: chromeStyle.space.s5

        Label {
            id: caption
            objectName: "popUpCaption"
            Layout.fillWidth: true
            Layout.leftMargin: chromeStyle.space.s5
            text: root._chosenIndex >= 0 ? root.labelAt(root._chosenIndex) : root.value
            font: chromeStyle.type.body
            color: chromeStyle.colors.controlText
            elide: Text.ElideRight
            // A value can be a family name or a path: text, never markup.
            textFormat: Text.PlainText
            Accessible.ignored: true
        }

        Label {
            id: chevron
            Layout.rightMargin: chromeStyle.space.s5
            text: chromeStyle.menuGlyph.length > 0 ? chromeStyle.menuGlyph : "▾"
            font: chromeStyle.type.body
            color: chromeStyle.colors.controlText
            Accessible.ignored: true
        }
    }

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    Popup {
        id: popup
        objectName: "popUpPopup"
        y: root.height
        width: Math.max(root.width, chromeStyle.settings.stepperField)
        padding: chromeStyle.space.s3
        modal: false
        focus: true
        // Placed in the window's own scene, as every popup here is (see PopupSurface.qml).
        popupType: Popup.Item
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        margins: chromeStyle.shadowMargin

        onOpened: {
            search.text = "";
            if (root.searchable)
                search.forceActiveFocus();
            else
                list.forceActiveFocus();
            list.currentIndex = Math.max(0, root._rows.findIndex(row => row.value === root.value));
        }

        background: PopupSurface {}

        contentItem: ColumnLayout {
            spacing: chromeStyle.space.s3

            Keys.onEscapePressed: popup.close()

            TextField {
                id: search
                objectName: "popUpSearch"
                Layout.fillWidth: true
                visible: root.searchable
                font: chromeStyle.type.body
                color: sys.windowText
                placeholderText: qsTr("Search")
                selectByMouse: true
                Accessible.name: qsTr("Search")
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onReturnPressed: list.chooseCurrent()
                Keys.onEscapePressed: popup.close()
            }

            ListView {
                id: list
                objectName: "popUpList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(root._rows.length, root.maxRows) * chromeStyle.settings.control
                clip: true
                model: root._rows
                currentIndex: 0
                boundsBehavior: Flickable.StopAtBounds

                function chooseCurrent() {
                    if (list.currentIndex >= 0 && list.currentIndex < root._rows.length) {
                        root.picked(root._rows[list.currentIndex].value);
                        popup.close();
                    }
                }

                Keys.onReturnPressed: list.chooseCurrent()
                Keys.onEscapePressed: popup.close()

                delegate: Rectangle {
                    id: rowItem
                    required property int index
                    required property var modelData
                    readonly property bool isCurrent: ListView.isCurrentItem
                    objectName: "popUpRow"
                    width: ListView.view.width
                    height: chromeStyle.settings.control
                    radius: chromeStyle.radii.control
                    color: rowItem.isCurrent ? chromeStyle.colors.settingsSelectionFill : "transparent"

                    Accessible.role: Accessible.ListItem
                    Accessible.name: rowItem.modelData.label
                    Accessible.selectable: true
                    Accessible.selected: rowItem.modelData.value === root.value

                    Label {
                        objectName: "popUpRowLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: chromeStyle.space.s5
                        anchors.rightMargin: chromeStyle.space.s5
                        text: rowItem.modelData.label
                        font: chromeStyle.type.body
                        color: rowItem.isCurrent ? chromeStyle.colors.settingsSelectionText : sys.windowText
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        Accessible.ignored: true
                    }

                    HoverHandler {
                        onHoveredChanged: if (hovered) list.currentIndex = rowItem.index
                    }
                    TapHandler {
                        onTapped: {
                            list.currentIndex = rowItem.index;
                            list.chooseCurrent();
                        }
                    }
                }
            }
        }
    }
}
