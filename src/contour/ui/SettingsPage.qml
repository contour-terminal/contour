// vim:syntax=qml
// The in-app settings page, shown in the content area in place of the terminal when the
// WindowController's settingsActive flag is set (see Main.qml). It binds entirely to a
// SettingsController (the editable bridge over the configuration) and a WindowController (to close the
// page). It never edits contour.yml: it creates/edits GUI-owned side files, and shows contour.yml /
// builtin entities read-only.
//
// Layout (Windows-Terminal style): a title bar, a left navigation rail with accent "pill" selection
// (grouped GENERAL / PROFILES / COLOR SCHEMES), and a right content pane with a page header band and
// elevated "setting cards". All colours follow the OS light/dark theme live via SystemPalette, the same
// idiom the command palette and tab flyouts use.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    // The SettingsController and the owning WindowController. Both null-guarded throughout: the
    // controllers are torn down before the QML tree on window close, and bindings re-evaluate once
    // against null during teardown (an unguarded access would raise a TypeError that fails the tests).
    property var controller: null
    property var windowController: null

    // Which editor the right pane shows: "profile", "scheme", "globals" or "keybindings".
    //
    // Opens on the global settings rather than on a chooser: the page is reached by asking for
    // settings, so landing on a pane that says "pick something on the left" spends the user's first
    // click telling them what they already came here to do. Global settings is the one pane that
    // always exists — a fresh install has no GUI-owned profile or colour scheme to show instead.
    property string editorMode: "globals"

    // Live OS palette handle, so the whole page follows dark/light in realtime (see CommandPalette.qml).
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.6)
    readonly property color hairline: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.12)

    color: sys.window

    readonly property var schemeNameList: {
        var out = []
        if (controller)
            for (var i = 0; i < controller.colorSchemes.length; ++i)
                out.push(controller.colorSchemes[i].name)
        return out
    }

    // The page header, driven by which editor is open.
    readonly property string headerTitle: {
        if (root.editorMode === "profile")
            return (root.controller && root.controller.editingProfile.length > 0)
                   ? qsTr("Profile · %1").arg(root.controller.editingProfile)
                   : qsTr("New profile")
        if (root.editorMode === "scheme")
            return (root.controller && root.controller.editingScheme.length > 0)
                   ? qsTr("Color scheme · %1").arg(root.controller.editingScheme)
                   : qsTr("New color scheme")
        if (root.editorMode === "globals")
            return qsTr("Global settings")
        if (root.editorMode === "keybindings")
            return qsTr("Keybindings")
        return ""
    }
    readonly property string headerSubtitle: {
        if (root.editorMode === "profile")
            return qsTr("Appearance and behaviour for this profile — saved to profiles/<name>.yml.")
        if (root.editorMode === "scheme")
            return qsTr("A reusable palette — saved to colorschemes/<name>.yml.")
        if (root.editorMode === "globals")
            return qsTr("Application-wide overrides — saved to settings.yml.")
        if (root.editorMode === "keybindings")
            return qsTr("Configured in contour.yml (read-only here).")
        return ""
    }

    // {{{ Field grouping and live filter
    //
    // The field panes are long — a profile has around ninety settings — so they are shown as collapsible
    // groups with a search box over them. The grouping is not decided here: each row of
    // SettingsController.profileFields carries the `group` and `groupGlyph` it belongs under, so which
    // fields belong together is a fact about the fields, stated next to them in C++.

    /// Live filter over the field panes. Matches a field's label, help text or key.
    property string filterText: ""
    readonly property bool filtering: root.filterText.trim().length > 0

    /// Per-group open state, keyed by group title. Absent means open.
    ///
    /// Held here rather than inside each SettingsSection because a keystroke in the search box changes
    /// the group model, which rebuilds every delegate — state living in a delegate would reset on every
    /// character typed.
    property var expandedGroups: ({})

    /// Whether the group titled @p title is currently open.
    function groupExpanded(title) {
        // A search opens whatever it matched: having to expand a group to see a hit you already searched
        // for defeats the search.
        if (root.filtering)
            return true
        return root.expandedGroups[title] !== false
    }

    /// Flips the open state of the group titled @p title.
    function toggleGroup(title) {
        // Reassigned as a fresh object, because mutating the existing one in place does not notify the
        // bindings that read it.
        var next = {}
        for (var key in root.expandedGroups)
            next[key] = root.expandedGroups[key]
        next[title] = (root.expandedGroups[title] === false)
        root.expandedGroups = next
    }

    /// Whether @p field passes the current filter.
    function fieldMatches(field) {
        var needle = root.filterText.trim().toLowerCase()
        if (needle.length === 0)
            return true
        // The key is searched too, so a user who knows the YAML name of a setting can type that.
        return String(field.label).toLowerCase().indexOf(needle) >= 0
            || String(field.help).toLowerCase().indexOf(needle) >= 0
            || String(field.key).toLowerCase().indexOf(needle) >= 0
    }

    /// Whether @p text passes the current filter. For the sections that are not lists of scalar fields
    /// (the colour scheme, the status line indicator) and so have only their title to match on.
    function textMatches(text) {
        var needle = root.filterText.trim().toLowerCase()
        return needle.length === 0 || String(text).toLowerCase().indexOf(needle) >= 0
    }

    /// Buckets @p rows into [{ title, glyph, fields }], keeping the order C++ listed them in and
    /// dropping groups the filter emptied.
    function groupFields(rows) {
        var out = []
        var byTitle = ({})
        for (var i = 0; i < rows.length; ++i) {
            var field = rows[i]
            if (!root.fieldMatches(field))
                continue
            var title = field.group !== undefined ? field.group : ""
            if (byTitle[title] === undefined) {
                byTitle[title] = {
                    "title": title,
                    "glyph": field.groupGlyph !== undefined ? field.groupGlyph : "",
                    "fields": []
                }
                out.push(byTitle[title])
            }
            byTitle[title].fields.push(field)
        }
        return out
    }

    readonly property var profileGroups: root.controller ? root.groupFields(root.controller.profileFields) : []
    readonly property var globalFieldsFiltered: {
        var out = []
        if (!root.controller)
            return out
        var rows = root.controller.globalFields
        for (var i = 0; i < rows.length; ++i)
            if (root.fieldMatches(rows[i]))
                out.push(rows[i])
        return out
    }
    /// How many fields the filter is currently hiding, for the "no matches" hint.
    readonly property int visibleFieldCount: {
        var total = 0
        for (var i = 0; i < root.profileGroups.length; ++i)
            total += root.profileGroups[i].fields.length
        return total
    }
    // }}}

    // Delete is confirmed through a modal dialog; the row that asked stashes its target here first.
    property string pendingDeleteKind: "" // "profile" | "scheme"
    property string pendingDeleteName: ""

    function requestDelete(kind, name) {
        root.pendingDeleteKind = kind
        root.pendingDeleteName = name
        deleteDialog.open()
    }

    ConfirmDialog {
        id: deleteDialog
        heading: qsTr("Delete “%1”").arg(root.pendingDeleteName)
        confirmText: qsTr("Delete")
        message: root.pendingDeleteKind === "profile"
                 ? qsTr("Delete the profile “%1”? This removes its GUI side file and cannot be undone.").arg(root.pendingDeleteName)
                 : qsTr("Delete the color scheme “%1”? This removes its GUI side file and cannot be undone.").arg(root.pendingDeleteName)
        onConfirmed: {
            if (!root.controller) return
            if (root.pendingDeleteKind === "profile")
                root.controller.deleteProfile(root.pendingDeleteName)
            else
                root.controller.deleteColorScheme(root.pendingDeleteName)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // {{{ Title bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 16
                spacing: 12

                Label {
                    text: qsTr("Settings")
                    font.pointSize: 18
                    font.weight: Font.DemiBold
                    color: sys.windowText
                    Layout.fillWidth: true
                }
                Rectangle {
                    objectName: "lockedBadge"
                    visible: root.controller && root.controller.locked
                    radius: 4
                    color: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.08)
                    Layout.preferredHeight: 24
                    Layout.preferredWidth: lockedLabel.implicitWidth + 20
                    Label {
                        id: lockedLabel
                        anchors.centerIn: parent
                        text: qsTr("🔒 read-only (gui_config_locked)")
                        color: root.subtleText
                        font.pointSize: 9
                    }
                }
                Button {
                    objectName: "closeSettingsButton"
                    text: qsTr("Close")
                    onClicked: if (root.windowController) root.windowController.closeSettings()
                }
            }

            Rectangle { // bottom hairline
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: root.hairline
            }
        }
        // }}}

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // {{{ Left navigation rail
            Rectangle {
                Layout.preferredWidth: 280
                Layout.minimumWidth: 280
                Layout.maximumWidth: 280
                Layout.fillHeight: true
                color: "transparent"

                ScrollView {
                    id: navScroll
                    anchors.fill: parent
                    contentWidth: availableWidth
                    clip: true

                    ColumnLayout {
                        width: navScroll.availableWidth
                        spacing: 4

                        // General
                        Label {
                            text: qsTr("GENERAL")
                            font.pointSize: 8
                            font.bold: true
                            font.letterSpacing: 1
                            color: root.subtleText
                            Layout.leftMargin: 16
                            Layout.topMargin: 12
                        }
                        SettingsNavItem {
                            objectName: "globalSettingsButton"
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            Layout.rightMargin: 8
                            text: qsTr("Global settings")
                            glyph: "⚙"
                            selected: root.editorMode === "globals"
                            onClicked: root.editorMode = "globals"
                        }
                        SettingsNavItem {
                            objectName: "keybindingsButton"
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            Layout.rightMargin: 8
                            text: qsTr("Keybindings")
                            glyph: "⌨"
                            selected: root.editorMode === "keybindings"
                            onClicked: root.editorMode = "keybindings"
                        }

                        // Profiles
                        Label {
                            text: qsTr("PROFILES")
                            font.pointSize: 8
                            font.bold: true
                            font.letterSpacing: 1
                            color: root.subtleText
                            Layout.leftMargin: 16
                            Layout.topMargin: 14
                        }
                        Repeater {
                            model: root.controller ? root.controller.profiles : []
                            delegate: SettingsListItem {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.leftMargin: 8
                                Layout.rightMargin: 8
                                entryName: modelData.name
                                showHome: true
                                isDefault: modelData.isDefault === true
                                editable: modelData.editable === true
                                locked: root.controller && root.controller.locked
                                selected: root.editorMode === "profile" && root.controller
                                          && root.controller.editingProfile === modelData.name
                                onActivated: {
                                    if (!root.controller) return
                                    root.controller.editProfile(modelData.name)
                                    root.editorMode = "profile"
                                }
                                onSetDefaultRequested: if (root.controller) root.controller.setDefaultProfile(modelData.name)
                                onRenameRequested: (newName) => { if (root.controller) root.controller.renameProfile(modelData.name, newName) }
                                onDeleteRequested: root.requestDelete("profile", modelData.name)
                            }
                        }
                        SettingsNavItem {
                            objectName: "newProfileButton"
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            Layout.rightMargin: 8
                            text: qsTr("New profile")
                            glyph: "＋"
                            accent: true
                            enabled: root.controller && !root.controller.locked
                            onClicked: {
                                root.controller.newProfile(root.controller.defaultProfile)
                                root.editorMode = "profile"
                            }
                        }

                        // Color schemes
                        Label {
                            text: qsTr("COLOR SCHEMES")
                            font.pointSize: 8
                            font.bold: true
                            font.letterSpacing: 1
                            color: root.subtleText
                            Layout.leftMargin: 16
                            Layout.topMargin: 14
                        }
                        Repeater {
                            model: root.controller ? root.controller.colorSchemes : []
                            delegate: SettingsListItem {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.leftMargin: 8
                                Layout.rightMargin: 8
                                entryName: modelData.name
                                showHome: false
                                editable: modelData.editable === true
                                locked: root.controller && root.controller.locked
                                selected: root.editorMode === "scheme" && root.controller
                                          && root.controller.editingScheme === modelData.name
                                onActivated: {
                                    if (!root.controller) return
                                    root.controller.editColorScheme(modelData.name)
                                    root.editorMode = "scheme"
                                }
                                onRenameRequested: (newName) => { if (root.controller) root.controller.renameColorScheme(modelData.name, newName) }
                                onDeleteRequested: root.requestDelete("scheme", modelData.name)
                            }
                        }
                        SettingsNavItem {
                            objectName: "newSchemeButton"
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            Layout.rightMargin: 8
                            text: qsTr("New color scheme")
                            glyph: "＋"
                            accent: true
                            enabled: root.controller && !root.controller.locked
                            onClicked: {
                                root.controller.newColorScheme("")
                                root.editorMode = "scheme"
                            }
                        }

                        Item { Layout.fillHeight: true; Layout.preferredHeight: 12 }
                    }
                }
            }
            // }}}

            Rectangle { // vertical rail separator
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: root.hairline
            }

            // {{{ Right content pane
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 20
                spacing: 14

                // Page header band.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: root.headerTitle
                        font.pointSize: 16
                        font.weight: Font.DemiBold
                        color: sys.windowText
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: root.headerSubtitle
                        visible: text.length > 0
                        color: root.subtleText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    // Search over the fields of whichever field pane is open. Only those two panes have
                    // fields to filter; the colour-scheme and keybinding panes have their own shapes.
                    Rectangle {
                        objectName: "fieldFilterBox"
                        visible: root.editorMode === "profile" || root.editorMode === "globals"
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Layout.preferredHeight: 34
                        radius: 8
                        color: sys.base
                        border.width: 1
                        border.color: filterField.activeFocus
                                      ? Qt.rgba(sys.highlight.r, sys.highlight.g, sys.highlight.b, 0.55)
                                      : root.hairline

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 6
                            spacing: 6

                            Label {
                                text: "🔍"
                                color: root.subtleText
                                font.pointSize: 9
                            }
                            TextField {
                                id: filterField
                                objectName: "fieldFilterField"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Search settings…")
                                selectByMouse: true
                                background: null // the surrounding Rectangle is the field's frame
                                Accessible.name: qsTr("Search settings")
                                // Bound live rather than on commit, so the list narrows as you type.
                                onTextChanged: root.filterText = text
                                Keys.onEscapePressed: clear()
                            }
                            ToolButton {
                                objectName: "fieldFilterClearButton"
                                visible: filterField.text.length > 0
                                text: "✕"
                                flat: true
                                Accessible.name: qsTr("Clear the search")
                                onClicked: filterField.clear()
                            }
                        }
                    }
                }

                // The editors: mutually exclusive, visible-toggled, each anchored to fill this pane.
                Item {
                    id: rightPane
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    // {{{ Profile editor
                    ColumnLayout {
                        anchors.fill: parent
                        visible: root.editorMode === "profile"
                        spacing: 12

                        Rectangle {
                            objectName: "readOnlyBanner"
                            visible: root.controller && root.controller.editingReadOnly
                            Layout.fillWidth: true
                            Layout.preferredHeight: readOnlyLabel.implicitHeight + 20
                            radius: 8
                            color: Qt.rgba(sys.highlight.r, sys.highlight.g, sys.highlight.b, 0.10)
                            border.width: 1
                            border.color: Qt.rgba(sys.highlight.r, sys.highlight.g, sys.highlight.b, 0.35)
                            Label {
                                id: readOnlyLabel
                                anchors.fill: parent
                                anchors.margins: 10
                                text: qsTr("🔒 This profile is defined in contour.yml and is read-only here. "
                                           + "Use \"Save As\" to create an editable copy.")
                                wrapMode: Text.WordWrap
                                verticalAlignment: Text.AlignVCenter
                                color: sys.windowText
                            }
                        }

                        ScrollView {
                            id: profileScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: profileScroll.availableWidth
                                spacing: 10

                                // One collapsible card per field group. Which fields share a card is
                                // decided in C++ (profileFieldGroups), not matched on here.
                                Repeater {
                                    model: root.profileGroups
                                    delegate: SettingsSection {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        title: modelData.title
                                        glyph: modelData.glyph
                                        badgeCount: modelData.fields.length
                                        expanded: root.groupExpanded(modelData.title)
                                        onToggleRequested: root.toggleGroup(modelData.title)

                                        Repeater {
                                            model: modelData.fields
                                            delegate: SettingRow {
                                                required property var modelData
                                                required property int index
                                                Layout.fillWidth: true
                                                flat: true
                                                showSeparator: index > 0
                                                fieldKey: modelData.key
                                                label: modelData.label
                                                help: modelData.help
                                                type: modelData.type
                                                value: modelData.value
                                                options: modelData.options
                                                editable: root.controller && !root.controller.editingReadOnly
                                                onEdited: (key, value) => root.controller.setProfileField(key, value)
                                            }
                                        }
                                    }
                                }

                                // Says so when a search matched nothing, rather than showing an empty pane.
                                Label {
                                    objectName: "noFieldMatchesLabel"
                                    visible: root.filtering && root.visibleFieldCount === 0
                                    Layout.fillWidth: true
                                    Layout.topMargin: 12
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    color: root.subtleText
                                    text: qsTr("No setting matches “%1”.").arg(root.filterText.trim())
                                }

                                // Color-scheme selection group (with dark/light distinction). A section
                                // like the field groups, so the pane reads as one list of groups; its
                                // body is hand-built because a scheme choice is not a scalar field.
                                SettingsSection {
                                    id: schemeSection
                                    Layout.fillWidth: true
                                    title: qsTr("Color scheme")
                                    glyph: "◐"
                                    visible: !root.filtering || root.textMatches(title)
                                    expanded: root.groupExpanded(title)
                                    onToggleRequested: root.toggleGroup(title)

                                    ColumnLayout {
                                        id: schemeGroup
                                        Layout.fillWidth: true
                                        spacing: 8

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Label { text: qsTr("Mode"); color: root.subtleText }
                                            ComboBox {
                                                id: schemeModeCombo
                                                objectName: "schemeModeCombo"
                                                enabled: root.controller && !root.controller.editingReadOnly
                                                model: [qsTr("Single"), qsTr("Light / Dark")]
                                                currentIndex: (root.controller && root.controller.colorSchemeMode === "dual") ? 1 : 0
                                                onActivated: if (root.controller)
                                                                 root.controller.setColorSchemeMode(currentIndex === 1 ? "dual" : "simple")
                                            }
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            visible: root.controller && root.controller.colorSchemeMode === "simple"
                                            spacing: 8
                                            Label { text: qsTr("Scheme"); color: root.subtleText }
                                            ComboBox {
                                                objectName: "simpleSchemeCombo"
                                                Layout.preferredWidth: 220
                                                enabled: root.controller && !root.controller.editingReadOnly
                                                model: root.schemeNameList
                                                currentIndex: root.controller ? root.schemeNameList.indexOf(root.controller.colorScheme) : -1
                                                onActivated: if (root.controller) root.controller.setColorScheme(currentText)
                                            }
                                        }
                                        GridLayout {
                                            Layout.fillWidth: true
                                            visible: root.controller && root.controller.colorSchemeMode === "dual"
                                            columns: 2
                                            columnSpacing: 8
                                            rowSpacing: 6
                                            Label { text: qsTr("Light"); color: root.subtleText }
                                            ComboBox {
                                                objectName: "lightSchemeCombo"
                                                Layout.preferredWidth: 220
                                                enabled: root.controller && !root.controller.editingReadOnly
                                                model: root.schemeNameList
                                                currentIndex: root.controller ? root.schemeNameList.indexOf(root.controller.colorSchemeLight) : -1
                                                onActivated: if (root.controller) root.controller.setColorSchemeLight(currentText)
                                            }
                                            Label { text: qsTr("Dark"); color: root.subtleText }
                                            ComboBox {
                                                objectName: "darkSchemeCombo"
                                                Layout.preferredWidth: 220
                                                enabled: root.controller && !root.controller.editingReadOnly
                                                model: root.schemeNameList
                                                currentIndex: root.controller ? root.schemeNameList.indexOf(root.controller.colorSchemeDark) : -1
                                                onActivated: if (root.controller) root.controller.setColorSchemeDark(currentText)
                                            }
                                        }
                                    }
                                }

                                // Status line indicator editor: a visual builder that replaces the three
                                // raw template fields the descriptor list used to expose.
                                SettingsSection {
                                    id: indicatorSection
                                    Layout.fillWidth: true
                                    title: qsTr("Status line indicator")
                                    glyph: "▤"
                                    visible: !root.filtering || root.textMatches(title)
                                    expanded: root.groupExpanded(title)
                                    onToggleRequested: root.toggleGroup(title)

                                    StatusLineIndicatorEditor {
                                        Layout.fillWidth: true
                                        controller: root.controller
                                        editable: root.controller && !root.controller.editingReadOnly
                                    }
                                }

                                Item { Layout.preferredHeight: 4 } // breathing room above the action bar
                            }
                        }

                        // Pinned action bar.
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: actionRow.implicitHeight + 20
                            radius: 8
                            color: sys.base
                            border.width: 1
                            border.color: root.hairline

                            RowLayout {
                                id: actionRow
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8
                                Button {
                                    objectName: "saveProfileButton"
                                    text: qsTr("Save")
                                    highlighted: true
                                    enabled: root.controller && !root.controller.locked
                                             && !root.controller.editingReadOnly
                                             && root.controller.editingProfile.length > 0
                                    onClicked: root.controller.saveProfile()
                                }
                                ToolSeparator {}
                                TextField {
                                    id: saveAsField
                                    objectName: "saveAsField"
                                    Layout.preferredWidth: 180
                                    placeholderText: qsTr("New profile name")
                                    selectByMouse: true
                                }
                                Button {
                                    objectName: "saveProfileAsButton"
                                    text: qsTr("Save As")
                                    enabled: root.controller && !root.controller.locked
                                             && saveAsField.text.trim().length > 0
                                    onClicked: {
                                        if (root.controller.saveProfileAs(saveAsField.text.trim()))
                                            saveAsField.clear()
                                    }
                                }
                                Item { Layout.fillWidth: true }
                                // Delete lives on the profile list row (hover trashcan / right-click),
                                // routed through the page's confirmation dialog.
                            }
                        }
                    }
                    // }}}

                    // Color-scheme editor.
                    ColorSchemeEditor {
                        anchors.fill: parent
                        visible: root.editorMode === "scheme"
                        controller: root.controller
                    }

                    // {{{ Global settings
                    ScrollView {
                        id: globalsScroll
                        anchors.fill: parent
                        visible: root.editorMode === "globals"
                        contentWidth: availableWidth
                        clip: true

                        ColumnLayout {
                            width: globalsScroll.availableWidth
                            spacing: 10

                            // A single section: there are only a handful of application-wide settings, so
                            // splitting them further would be more chrome than content. Presented as a
                            // section anyway, so both field panes look like the same kind of page.
                            SettingsSection {
                                id: globalsSection
                                Layout.fillWidth: true
                                title: qsTr("Application")
                                glyph: "⚙"
                                badgeCount: root.globalFieldsFiltered.length
                                expanded: root.groupExpanded(title)
                                onToggleRequested: root.toggleGroup(title)

                                Repeater {
                                    model: root.globalFieldsFiltered
                                    delegate: RowLayout {
                                        required property var modelData
                                        required property int index
                                        Layout.fillWidth: true
                                        spacing: 8
                                        SettingRow {
                                            Layout.fillWidth: true
                                            flat: true
                                            showSeparator: index > 0
                                            fieldKey: modelData.key
                                            label: modelData.label
                                            help: modelData.help
                                            type: modelData.type
                                            value: modelData.value
                                            options: modelData.options
                                            editable: root.controller && !root.controller.locked
                                            onEdited: (key, value) => root.controller.setGlobalField(key, value)
                                        }
                                        Button {
                                            text: qsTr("Reset")
                                            visible: modelData.overridden
                                            enabled: root.controller && !root.controller.locked
                                            onClicked: root.controller.resetGlobalField(modelData.key)
                                        }
                                    }
                                }
                            }

                            Label {
                                objectName: "noGlobalMatchesLabel"
                                visible: root.filtering && root.globalFieldsFiltered.length === 0
                                Layout.fillWidth: true
                                Layout.topMargin: 12
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                color: root.subtleText
                                text: qsTr("No setting matches “%1”.").arg(root.filterText.trim())
                            }
                        }
                    }
                    // }}}

                    // {{{ Keybindings (read-only viewer)
                    Rectangle {
                        anchors.fill: parent
                        visible: root.editorMode === "keybindings"
                        radius: 8
                        color: sys.base
                        border.width: 1
                        border.color: root.hairline
                        clip: true

                        ListView {
                            objectName: "keybindingsList"
                            anchors.fill: parent
                            anchors.margins: 6
                            clip: true
                            model: root.controller ? root.controller.keybindings : []
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Item {
                                required property var modelData
                                width: ListView.view.width
                                height: 34
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 12
                                    Label {
                                        text: modelData.trigger
                                        Layout.preferredWidth: 200
                                        font.family: "monospace"
                                        color: sys.windowText
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        text: modelData.action
                                        Layout.fillWidth: true
                                        color: sys.windowText
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        text: modelData.mode
                                        color: root.subtleText
                                        Layout.preferredWidth: 90
                                    }
                                }
                                Rectangle {
                                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                                    height: 1
                                    color: root.hairline
                                    opacity: 0.5
                                }
                            }
                        }
                    }
                    // }}}
                }
            }
            // }}}
        }
    }
}
