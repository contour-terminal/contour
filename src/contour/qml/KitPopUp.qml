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
import QtQuick.Window

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
    readonly property real _contentGaps: caption.Layout.leftMargin + root.contentItem.spacing
                                          + chevron.Layout.rightMargin
    // The widest label of the options, as the labels are laid out: the button and the list are wide enough
    // for it, so no option is ever cut off or elided, whichever one is chosen. A list that searches (the font
    // picker's) holds hundreds of entries, which are not each measured: it is as wide as what is chosen.
    property real _widestLabel: 0

    /// Measures the labels again for the widest.
    function measure() {
        let widest = 0;
        for (let index = 0; index < labelMetrics.count; ++index) {
            // An item is not there yet while it is being added.
            const measured = labelMetrics.itemAt(index);
            if (measured)
                widest = Math.max(widest, Math.ceil(measured.implicitWidth));
        }
        root._widestLabel = widest;
    }
    // What a row of the list needs beside its label: the margin at each side.
    // The widest label of the list, measured whenever it is about to open (a font list holds hundreds of
    // entries, measured once, not per row). Measured through a Label set up as the rows' are, never through
    // the font's metrics: a row sizes itself by its Label's implicitWidth, and the two differ by a pixel on
    // some platforms.
    property real _widestRow: 0

    /// Measures every option's label once, for the list's width.
    function measureRows() {
        let widest = 0;
        for (let index = 0; index < root.options.length; ++index) {
            rowProbe.text = root.labelAt(index);
            widest = Math.max(widest, rowProbe.implicitWidth);
        }
        root._widestRow = Math.ceil(widest);
    }
    readonly property real _rowInset: chromeStyle.space.s5 + chromeStyle.space.s5

    /// Opens the list.
    function open() {
        if (root.enabled)
            popup.open();
    }

    enabled: root.editable
    opacity: root.enabled ? 1 : chromeStyle.settings.disabledOpacity
    activeFocusOnTab: true
    // Whole pixels, and a device pixel over: the layout snaps its items to pixels, and a caption snapped
    // below its text elides it.
    implicitWidth: Math.ceil(Math.max(root._widestLabel, caption.implicitWidth)
                             + chevron.implicitWidth + root._contentGaps
                             + chromeStyle.hairline(Screen.devicePixelRatio))
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

    // One hidden Label per option, to measure the label as the caption and the rows lay it out.
    Repeater {
        id: labelMetrics
        model: root.searchable ? 0 : root.options.length
        onItemAdded: root.measure()
        onItemRemoved: root.measure()

        Label {
            required property int index
            visible: false
            font: chromeStyle.type.body
            text: root.labelAt(index)
            textFormat: Text.PlainText
            onImplicitWidthChanged: root.measure()
            Accessible.ignored: true
        }
    }

    // The one hidden Label every option's text is put through to measure a row, as a row's Label lays it out.
    Label {
        id: rowProbe
        visible: false
        font: chromeStyle.type.body
        textFormat: Text.PlainText
        Accessible.ignored: true
    }

    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    Popup {
        id: popup
        objectName: "popUpPopup"
        y: root.height
        // At least the button's width and the widest row's, within the window.
        width: Math.min(Math.max(root.width, chromeStyle.settings.stepperField,
                                 Math.max(root._widestLabel, root._widestRow) + root._rowInset
                                 + popup.leftPadding + popup.rightPadding),
                        root.Window.width > 0 ? root.Window.width - popup.leftMargin - popup.rightMargin : Number.POSITIVE_INFINITY)
        padding: chromeStyle.space.s3
        modal: false
        focus: true
        // Placed in the window's own scene, as every popup here is (see PopupSurface.qml).
        popupType: Popup.Item
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        margins: chromeStyle.shadowMargin

        onAboutToShow: root.measureRows()

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
                contentWidth: root._widestRow + root._rowInset
                flickableDirection: Flickable.HorizontalAndVerticalFlick
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
                    // As wide as the widest row: a name too long for the window scrolls, never elides.
                    width: Math.max(ListView.view.width, ListView.view.contentWidth)
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
