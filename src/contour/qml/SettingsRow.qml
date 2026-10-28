// vim:syntax=qml
// One setting in a Settings group: its label, its help line and what is wrong with its value on the left, the
// kit control that edits it on the right, and "Modified · Reset" beside it where settings.yml overrides the
// value.
//
// The row is data-driven: `field` is a row of the controller's lists (SettingsController::globalFields and
// profileFields), and its `controlName` picks the control, so a new kind of setting is a new case in the
// table below and no change to a page. The control is named "settingsControl:<key>"; the row is
// "settingsRow:<key>".
//
// It sends a value only when it differs from the field's: choosing the entry already chosen, or leaving a
// field after Return, is no change. The row never holds a value of its own, so a change the page cannot save
// (the controller refuses it, or the write fails) shows the stored value again at once.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    /// The field this row edits: one row of the controller's lists.
    property var field: ({})
    /// Whether the user can change it.
    property bool editable: true
    /// The families the font picker lists.
    property var fontFamilies: []
    /// Whether a separator is drawn under the row, where the group is one card.
    property bool showSeparator: false

    /// The user asked for @p value.
    signal committed(string key, var value)
    /// The user asked for the override of @p key to be dropped.
    signal resetRequested(string key)

    // What the field names, over what a row of the controller's lists always carries: a test or a page built
    // by hand may name less.
    readonly property var _defaults: ({ key: "", label: "", help: "", problem: "", value: "", overridden: false,
                                        options: [], optionLabels: [], min: -Infinity, max: Infinity,
                                        step: 1, decimals: 0, unitText: "", displayFactor: 1,
                                        readout: "" })
    readonly property var _data: Object.assign({}, root._defaults, root.field)
    // The 32-bit range an integer field without bounds of its own holds, as the loader reads one.
    readonly property real _intLimit: 2147483647
    // The row keeps a gap above and below its text, and a padding at each side.
    readonly property real _textInset: 2 * chromeStyle.space.s3
    readonly property real _sidePadding: 2 * chromeStyle.settings.rowPadding
    // A text field is as wide as this many number fields.
    readonly property int _textFieldWidths: 2
    readonly property bool _hasHelp: root._data.help !== undefined && root._data.help.length > 0
    readonly property bool _separateCards: chromeStyle.settingsTraits.rows === chromeStyle.settingsTraits.separateCards
    readonly property string _controlName: root._data.controlName !== undefined ? root._data.controlName : "textField"

    /// @return @p base at @p pixels, or as it is where @p pixels is 0 (the chrome font's own size).
    function typed(base, pixels) {
        return pixels > 0 ? Qt.font({ family: base.family, weight: base.weight, pixelSize: pixels }) : base;
    }

    /// Sends @p value unless it is the field's.
    function commit(value) {
        if (value !== root._data.value)
            root.committed(root._data.key, value);
    }

    /// @return The number @p limit where it is a bound, else @p fallback.
    function bound(limit, fallback) {
        return Number.isFinite(limit) ? limit : fallback;
    }

    objectName: "settingsRow:" + root._data.key
    implicitHeight: Math.max(root._hasHelp ? chromeStyle.settings.rowWithHelp : chromeStyle.settings.row,
                             texts.implicitHeight + root._textInset)
    implicitWidth: texts.implicitWidth + root._sidePadding

    // The card of a row, where each row is its own; a grouped list draws one behind all its rows.
    Rectangle {
        anchors.fill: parent
        visible: root._separateCards
        radius: chromeStyle.settings.groupRadius
        color: chromeStyle.colors.settingsGroupFill
    }

    Rectangle {
        objectName: "settingsRowSeparator"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: chromeStyle.settings.rowPadding
        height: chromeStyle.borderWidth
        visible: root.showSeparator && !root._separateCards
        color: chromeStyle.colors.settingsSeparator
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: chromeStyle.settings.rowPadding
        anchors.rightMargin: chromeStyle.settings.rowPadding
        spacing: chromeStyle.space.s6

        ColumnLayout {
            id: texts
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: chromeStyle.space.s1

            Label {
                objectName: "settingsRowLabel"
                Layout.fillWidth: true
                text: root._data.label !== undefined ? root._data.label : ""
                font: root.typed(chromeStyle.type.body, chromeStyle.settings.labelType)
                color: chromeStyle.colors.settingsLabel
                opacity: root.editable ? 1 : chromeStyle.settings.disabledOpacity
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                Accessible.ignored: true
            }

            Label {
                objectName: "settingsRowHelp"
                Layout.fillWidth: true
                visible: root._hasHelp
                text: root._hasHelp ? root._data.help : ""
                font: root.typed(chromeStyle.type.footnote, chromeStyle.settings.helpType)
                color: chromeStyle.colors.settingsHelp
                opacity: root.editable ? 1 : chromeStyle.settings.disabledOpacity
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                Accessible.ignored: true
            }

            // What is wrong with the value, under the help: a setting the reader would refuse.
            Label {
                objectName: "settingsRowProblem"
                Layout.fillWidth: true
                visible: text.length > 0
                text: root._data.problem !== undefined ? root._data.problem : ""
                font: root.typed(chromeStyle.type.footnote, chromeStyle.settings.helpType)
                // The danger ink: the status hue on the native rows, and on the Terminal row the scheme's where
                // it reads on the bar.
                color: chromeStyle.statusInk(chromeStyle.statusDanger)
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
        }

        Button {
            objectName: "modifiedResetButton:" + root._data.key
            Layout.alignment: Qt.AlignVCenter
            visible: root._data.overridden === true
            enabled: root.editable
            flat: true
            text: qsTr("Modified · Reset")
            font: chromeStyle.type.footnote
            palette.buttonText: chromeStyle.colors.settingsModified
            Accessible.name: qsTr("Reset %1").arg(root._data.label)
            onClicked: root.resetRequested(root._data.key)
        }

        Loader {
            id: control
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            sourceComponent: {
                switch (root._controlName) {
                case "switch": return switchControl;
                case "segmented": return segmentedControl;
                case "popUp": return popUpControl;
                case "stepper": return stepperControl;
                case "slider": return sliderControl;
                case "fontPicker": return fontPickerControl;
                case "colorWell": return colorWellControl;
                default: return textControl;
                }
            }
            // A field may name its control (a page's tests and a screen reader's rotor look it up by name).
            onLoaded: item.objectName = root._data.controlObjectName !== undefined
                                        ? root._data.controlObjectName : "settingsControl:" + root._data.key
        }
    }

    Component {
        id: switchControl
        KitSwitch {
            checked: root._data.value === true
            name: root._data.label
            description: root._data.help
            editable: root.editable
            onToggled: checked => root.commit(checked)
        }
    }

    Component {
        id: segmentedControl
        KitSegmented {
            options: root._data.options
            labels: root._data.optionLabels
            value: String(root._data.value)
            name: root._data.label
            description: root._data.help
            editable: root.editable
            onPicked: value => root.commit(value)
        }
    }

    Component {
        id: popUpControl
        KitPopUp {
            options: root._data.options
            labels: root._data.optionLabels
            value: String(root._data.value)
            name: root._data.label
            description: root._data.help
            editable: root.editable
            onPicked: value => root.commit(value)
        }
    }

    Component {
        id: stepperControl
        KitStepper {
            value: Number(root._data.value)
            from: root.bound(root._data.min, -root._intLimit)
            to: root.bound(root._data.max, root._intLimit)
            stepSize: root._data.step
            decimals: root._data.decimals
            unitText: root._data.unitText
            name: root._data.label
            description: root._data.help
            editable: root.editable
            onCommitted: value => root.commit(value)
        }
    }

    Component {
        id: sliderControl
        KitSlider {
            value: Number(root._data.value)
            from: root._data.min
            to: root._data.max
            stepSize: root._data.step
            decimals: root._data.decimals
            displayFactor: root._data.displayFactor
            unitText: root._data.unitText
            name: root._data.label
            description: root._data.help
            editable: root.editable
            onCommitted: value => root.commit(value)
        }
    }

    Component {
        id: fontPickerControl
        KitFontPicker {
            family: String(root._data.value)
            families: root.fontFamilies
            name: root._data.label
            description: root._data.help
            editable: root.editable
            onPicked: family => root.commit(family)
        }
    }

    Component {
        id: colorWellControl
        KitColorWell {
            hex: String(root._data.value)
            readout: root._data.readout
            readoutFails: root._data.readoutFails === true
            commitsUnchanged: root._data.commitsUnchanged === true
            name: root._data.label
            description: root._data.help
            editable: root.editable
            // A colour the caller derives is pinned by committing the one it shows, which is no change of value.
            onCommitted: hex => root._data.commitsUnchanged === true ? root.committed(root._data.key, hex)
                                                                     : root.commit(hex)
        }
    }

    // A free text field: Return or leaving it commits, Escape puts the stored text back.
    Component {
        id: textControl
        Rectangle {
            id: box
            implicitWidth: chromeStyle.settings.stepperField * root._textFieldWidths
            implicitHeight: chromeStyle.settings.control
            radius: chromeStyle.radii.control
            color: chromeStyle.colors.controlFill
            opacity: root.editable ? 1 : chromeStyle.settings.disabledOpacity
            enabled: root.editable

            Accessible.role: Accessible.EditableText
            Accessible.name: root._data.label
            Accessible.description: root._data.help

            TextField {
                id: entry
                anchors.fill: parent
                background: null
                font: chromeStyle.type.body
                color: chromeStyle.colors.controlText
                selectByMouse: true
                text: root._data.value === undefined || root._data.value === null ? "" : String(root._data.value)
                Accessible.ignored: true
                Keys.onEscapePressed: {
                    entry.text = Qt.binding(() => root._data.value === undefined ? "" : String(root._data.value));
                    entry.focus = false;
                }
                onEditingFinished: {
                    // Shown again before the text is sent: the page may rebuild this row in answer.
                    const typed = entry.text;
                    entry.text = Qt.binding(() => root._data.value === undefined ? "" : String(root._data.value));
                    root.commit(typed);
                }
            }
        }
    }
}
