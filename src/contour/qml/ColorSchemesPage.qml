// vim:syntax=qml
// A profile's Colors page: which scheme (or light and dark pair) the profile draws with, a gallery of every
// scheme to open one, and the editor of the open scheme -- its slots as colour wells, each with the contrast
// of its colour against what it sits on.
//
// What it edits is the controller: `colorScheme*` for the profile's choice (the profile draft, read-only
// where the profile is), and `editingScheme`, `schemeColors` and `schemeGallery` for the scheme being edited.
// The scheme is a file of its own, so it is edited whatever the profile allows; only a scheme Settings made
// takes a change, and any other offers a copy. Creating, renaming and deleting a scheme live here too:
// "+ New scheme" copies the one open, and a scheme a profile still draws with is neither deleted nor, where
// contour.yml defines that profile, renamed -- the controller refuses and the page shows why.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SettingsGroupsPage {
    id: root
    objectName: "colorSchemesPage"

    /// The SettingsController.
    property var controller: null

    /// A scheme's deletion was asked for; the page that owns the confirmation asks the user.
    signal deleteRequested(string name)

    // The profile's choice is the profile's to change, as everywhere on a profile page.
    readonly property bool _profileEditable: root.controller !== null && !root.controller.locked
                                             && !root.controller.editingReadOnly
    readonly property var _gallery: root.controller ? root.controller.schemeGallery : []
    readonly property var _schemeNames: root.controller ? root.controller.colorSchemes.map(scheme => scheme.name) : []
    readonly property string _editing: root.controller ? root.controller.editingScheme : ""
    // The scheme open for editing, as the gallery shows it: a copy staged and not yet written is there too.
    readonly property var _editingCard: root._gallery.find(card => card.name === root._editing)
    readonly property bool _schemeEditable: root._editingCard !== undefined && root._editingCard.editable
    readonly property bool _unlocked: root.controller !== null && !root.controller.locked
    // The schemes the profile draws with: the one, or the light and the dark.
    readonly property var _chosen: {
        if (!root.controller)
            return [];
        return root.controller.colorSchemeMode === "dual"
               ? [root.controller.colorSchemeLight, root.controller.colorSchemeDark]
               : [root.controller.colorScheme];
    }
    // What the slot blocks are called, by the token the controller gives them.
    readonly property var _slotBlocks: [
        { token: "base", title: qsTr("Base") },
        { token: "normal", title: qsTr("Normal colors") },
        { token: "bright", title: qsTr("Bright colors") },
        { token: "interface", title: qsTr("Gutter, folds and command blocks") }
    ]
    readonly property var _slotRows: root.controller ? root.controller.schemeColors : []
    // What a group's padding takes of its width and height, at both edges.
    readonly property real _padding: 2 * chromeStyle.settings.rowPadding

    /// @return The field row of slot @p slot for a SettingsRow: a colour well with its readout.
    function slotField(slot) {
        const reading = slot.against.length > 0 ? root.controller.contrastReadout(slot.color, slot.against) : null;
        return {
            key: "scheme." + slot.key,
            label: slot.label,
            help: slot.colorSource === "derived" ? qsTr("Follows the scheme's other colors") : "",
            value: slot.color,
            controlName: "colorWell",
            overridden: slot.colorSource === "pinned",
            commitsUnchanged: slot.colorSource === "derived",
            readout: reading ? reading.text : "",
            readoutFails: reading ? !reading.passes : false
        };
    }

    /// Opens the scheme the profile draws with, the dark half of a pair, for editing.
    function openChosenScheme() {
        if (root.controller && root._chosen.length > 0 && root._chosen[root._chosen.length - 1].length > 0)
            root.controller.editColorScheme(root._chosen[root._chosen.length - 1]);
    }

    // A slot's well commits its colour; a pinned slot's "Modified · Reset" lets it follow the others again.
    onCommitted: (key, value) => root.controller.commitSchemeColor(key.replace(/^scheme\./, ""), value)
    onResetRequested: key => root.controller.resetSchemeColor(key.replace(/^scheme\./, ""))
    Component.onCompleted: root.openChosenScheme()

    KitDialog {
        id: renameDialog
        objectName: "renameSchemeDialog"
        heading: qsTr("Rename “%1”").arg(root._editing)
        acceptText: qsTr("Rename")
        acceptEnabled: renameField.text.trim().length > 0
        onOpened: {
            renameField.text = root._editing;
            renameField.selectAll();
            renameField.forceActiveFocus();
        }
        onAccepted: if (root.controller) root.controller.requestRenameScheme(root._editing, renameField.text.trim())
        bodyData: [
            TextField {
                id: renameField
                objectName: "renameSchemeField"
                Layout.fillWidth: true
                Accessible.name: qsTr("New name")
                onAccepted: renameDialog.pressDefault()
            }
        ]
    }

    header: [
        // Which scheme the profile draws with.
        SettingsGroup {
            objectName: "schemeChoiceGroup"
            Layout.fillWidth: true
            title: qsTr("Color scheme")

            SettingsRow {
                Layout.fillWidth: true
                showSeparator: true
                editable: root._profileEditable
                field: ({ key: "color_scheme_mode",
                          label: qsTr("Mode"),
                          help: "",
                          controlName: "segmented",
                          options: ["simple", "dual"],
                          optionLabels: [qsTr("Single"), qsTr("Light / Dark")],
                          value: root.controller ? root.controller.colorSchemeMode : "simple" })
                onCommitted: (key, value) => root.controller.setColorSchemeMode(value)
            }
            SettingsRow {
                Layout.fillWidth: true
                visible: root.controller && root.controller.colorSchemeMode === "simple"
                editable: root._profileEditable
                field: ({ key: "color_scheme",
                          label: qsTr("Scheme"),
                          help: "",
                          controlName: "popUp",
                          options: root._schemeNames,
                          optionLabels: root._schemeNames,
                          value: root.controller ? root.controller.colorScheme : "" })
                onCommitted: (key, value) => root.controller.setColorScheme(value)
            }
            SettingsRow {
                Layout.fillWidth: true
                visible: root.controller && root.controller.colorSchemeMode === "dual"
                showSeparator: true
                editable: root._profileEditable
                field: ({ key: "color_scheme_light",
                          label: qsTr("Light"),
                          help: "",
                          controlName: "popUp",
                          options: root._schemeNames,
                          optionLabels: root._schemeNames,
                          value: root.controller ? root.controller.colorSchemeLight : "" })
                onCommitted: (key, value) => root.controller.setColorSchemeLight(value)
            }
            SettingsRow {
                Layout.fillWidth: true
                visible: root.controller && root.controller.colorSchemeMode === "dual"
                editable: root._profileEditable
                field: ({ key: "color_scheme_dark",
                          label: qsTr("Dark"),
                          help: "",
                          controlName: "popUp",
                          options: root._schemeNames,
                          optionLabels: root._schemeNames,
                          value: root.controller ? root.controller.colorSchemeDark : "" })
                onCommitted: (key, value) => root.controller.setColorSchemeDark(value)
            }
        },

        // Every scheme, as a card: opening one edits it below.
        SettingsGroup {
            objectName: "schemeGalleryGroup"
            Layout.fillWidth: true
            title: qsTr("Schemes")

            Item {
                Layout.fillWidth: true
                implicitHeight: gallery.height + root._padding

                Flow {
                    id: gallery
                    objectName: "schemeGallery"
                    x: chromeStyle.settings.rowPadding
                    y: chromeStyle.settings.rowPadding
                    width: parent.width - root._padding
                    spacing: chromeStyle.space.s6

                    Repeater {
                        model: root._gallery

                        delegate: SchemeCard {
                            required property var modelData
                            card: modelData
                            isEditing: modelData.name === root._editing
                            isChosen: root._chosen.includes(modelData.name)
                            onOpened: root.controller.editColorScheme(modelData.name)
                        }
                    }
                }
            }
        },

        // What can be done with the open scheme.
        SettingsGroup {
            objectName: "schemeActionsGroup"
            Layout.fillWidth: true
            visible: root._editing.length > 0
            title: root._editing

            Item {
                Layout.fillWidth: true
                implicitHeight: actions.implicitHeight + root._padding

                Flow {
                    id: actions
                    x: chromeStyle.settings.rowPadding
                    y: chromeStyle.settings.rowPadding
                    width: parent.width - root._padding
                    spacing: chromeStyle.space.s6

                    Button {
                        objectName: "newSchemeButton"
                        enabled: root._unlocked
                        text: qsTr("+ New scheme")
                        font: chromeStyle.type.body
                        Accessible.name: text
                        Accessible.description: qsTr("Makes a copy of “%1” to edit.").arg(root._editing)
                        onClicked: root.controller.duplicateScheme(root._editing)
                    }
                    Button {
                        objectName: "duplicateSchemeButton"
                        visible: root._unlocked && !root._schemeEditable
                        text: qsTr("Duplicate to Edit")
                        font: chromeStyle.type.body
                        Accessible.name: text
                        onClicked: root.controller.duplicateScheme(root._editing)
                    }
                    Button {
                        objectName: "renameSchemeButton"
                        visible: root._schemeEditable
                        text: qsTr("Rename…")
                        font: chromeStyle.type.body
                        Accessible.name: text
                        onClicked: renameDialog.open()
                    }
                    Button {
                        objectName: "deleteSchemeButton"
                        visible: root._schemeEditable
                        text: qsTr("Delete…")
                        font: chromeStyle.type.body
                        Accessible.name: text
                        onClicked: root.deleteRequested(root._editing)
                    }
                }
            }
        },

        // The slots of the open scheme, a block at a time.
        Repeater {
            model: root._editing.length > 0 ? root._slotBlocks : []

            delegate: SettingsGroup {
                id: block
                required property var modelData
                readonly property var slots: root._slotRows.filter(slot => slot.group === block.modelData.token)
                objectName: "schemeSlots:" + block.modelData.token
                Layout.fillWidth: true
                title: block.modelData.title
                visible: block.slots.length > 0

                Repeater {
                    model: block.slots

                    delegate: SettingsRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        field: root.slotField(modelData)
                        editable: root._schemeEditable
                        showSeparator: index < block.slots.length - 1
                        onCommitted: (key, value) => root.committed(key, value)
                        onResetRequested: key => root.resetRequested(key)
                    }
                }
            }
        }
    ]
}
