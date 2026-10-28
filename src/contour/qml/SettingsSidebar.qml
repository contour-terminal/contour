// vim:syntax=qml
// The Settings sidebar: a search field, the sections, the profiles and "Edit contour.yml…", and the "+ − …"
// buttons that manage the profiles.
//
// Typing into the search field replaces the rows with what `controller.searchSettings` finds; Return or a
// click on a result opens its page, Escape clears the search. The keyboard moves through the rows with Up and
// Down, each of which selects the row it lands on; the first press after the list takes the focus lands on
// the first row.
//
// It decides nothing: every opening goes out through `openRequested`, `editConfigurationRequested`,
// `addProfileRequested`, `removeProfileRequested` and `profileActionRequested`.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "settingsSidebar"

    /// The SettingsController.
    property var controller: null
    /// The page shown: "section:<id>", "profile:<name>" or one of its sub-pages.
    property string currentPage: ""
    /// Whether the page header is one bar across the sidebar and the page, each owning a half of it: the
    /// search field then sits level with the title.
    property bool searchInHeader: false
    /// Whether the user can add and remove profiles.
    property bool editable: true
    /// The glyph of each SettingsIcon, in enumerator order (Gear, Paintbrush, Keyboard, Terminal, Person,
    /// Document).
    readonly property var iconGlyphs: ["⚙", "✎", "⌨", "❯", "☺", "☰"]

    /// A row was asked to open page @p page.
    signal openRequested(string page)
    /// A search result was asked to open.
    signal resultRequested(var result)
    /// "Edit contour.yml…" was activated.
    signal editConfigurationRequested()
    /// "+" was pressed.
    signal addProfileRequested()
    /// "−" was pressed.
    signal removeProfileRequested()
    /// An entry of the profile menu was chosen: "duplicate", "rename" or "default".
    signal profileActionRequested(string action)

    readonly property bool searching: searchField.text.trim().length > 0
    readonly property var results: root.searching && root.controller ? root.controller.searchSettings(searchField.text) : []
    // The row the keyboard stands on; -1 until an arrow key lands on the first one.
    property int _cursor: -1

    /// Gives the search field the focus.
    function focusSearch() {
        searchField.forceActiveFocus();
        searchField.selectAll();
    }

    /// @return Whether the row @p row is the page shown.
    function isSelected(row) {
        if (row.kind === "section")
            return root.currentPage === "section:" + row.id;
        if (row.kind === "profile")
            return root.currentPage === "profile:" + row.name || root.currentPage.indexOf("profile:" + row.name + ":") === 0;
        return false;
    }

    /// Opens the row at @p index.
    function activate(index) {
        const row = root._rows[index];
        if (row.kind === "section")
            root.openRequested("section:" + row.id);
        else if (row.kind === "profile")
            root.openRequested("profile:" + row.name);
        else
            root.editConfigurationRequested();
    }

    // The sidebar's rows: the sections, the profiles, then "Edit contour.yml…".
    readonly property var _rows: root.controller ? root.controller.sidebar : []

    /// The profile row selected, or an empty string.
    readonly property string selectedProfile: {
        const found = root._rows.find(row => row.kind === "profile" && root.isSelected(row));
        return found ? found.name : "";
    }

    color: chromeStyle.colors.settingsSidebarFill
    implicitWidth: chromeStyle.settings.sidebarWidth

    Shortcut {
        sequences: [StandardKey.Find]
        context: Qt.WindowShortcut
        onActivated: root.focusSearch()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: chromeStyle.space.s4
        spacing: chromeStyle.space.s4

        // Level with the page's header where the header runs across the sidebar's half too.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: root.searchInHeader ? chromeStyle.settings.headerBar
                                                        : chromeStyle.settings.searchField

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: chromeStyle.settings.searchField
                radius: chromeStyle.radii.control
                color: chromeStyle.colors.controlFill

                TextField {
                    id: searchField
                    objectName: "settingsSearchField"
                    anchors.fill: parent
                    background: null
                    placeholderText: qsTr("Search")
                    font: chromeStyle.type.body
                    color: chromeStyle.colors.controlText
                    // The field's own ink: the style's default is the palette's text at half strength, which is
                    // unreadable on the Terminal row's pane-coloured field, and no softer tone reads on every scheme
                    // (solarized-dark's ink is barely 4.5:1 on its field).
                    placeholderTextColor: chromeStyle.colors.controlText
                    selectByMouse: true
                    Accessible.name: qsTr("Search settings")

                    Keys.onEscapePressed: searchField.clear()
                    Keys.onDownPressed: resultList.incrementCurrentIndex()
                    Keys.onUpPressed: resultList.decrementCurrentIndex()
                    Keys.onReturnPressed: resultList.openCurrent()
                }
            }
        }

        // What the search found.
        ListView {
            id: resultList
            objectName: "settingsSearchResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.searching
            clip: true
            model: root.results
            currentIndex: 0
            boundsBehavior: Flickable.StopAtBounds

            function openCurrent() {
                if (currentIndex >= 0 && currentIndex < root.results.length)
                    root.resultRequested(root.results[currentIndex]);
            }

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Search results")

            delegate: Rectangle {
                id: hit
                required property int index
                required property var modelData
                objectName: "searchResult"
                width: ListView.view.width
                height: chromeStyle.settings.sidebarRow
                radius: chromeStyle.radii.control
                color: ListView.isCurrentItem ? chromeStyle.colors.settingsSelectionFill : "transparent"

                Accessible.role: Accessible.ListItem
                Accessible.name: hit.modelData.label

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: chromeStyle.space.s5
                    anchors.rightMargin: chromeStyle.space.s5
                    verticalAlignment: Text.AlignVCenter
                    text: hit.modelData.label
                    font: chromeStyle.type.body
                    color: hit.ListView.isCurrentItem ? chromeStyle.colors.settingsSelectionText
                                                      : chromeStyle.colors.settingsLabel
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    Accessible.ignored: true
                }

                TapHandler {
                    onTapped: {
                        resultList.currentIndex = hit.index;
                        resultList.openCurrent();
                    }
                }
            }
        }

        Label {
            objectName: "settingsNoResults"
            Layout.fillWidth: true
            visible: root.searching && root.results.length === 0
            text: qsTr("No setting matches “%1”.").arg(searchField.text.trim())
            font: chromeStyle.type.footnote
            color: chromeStyle.colors.settingsHelp
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }

        // The sections, the profiles and "Edit contour.yml…". A column of rows rather than a ListView: the
        // rows are few, and every one of them exists, shown or not, for the keyboard and for a screen reader.
        Flickable {
            id: sidebarList
            objectName: "settingsSidebarList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.searching
            clip: true
            activeFocusOnTab: true
            contentWidth: width
            contentHeight: rowsColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Settings sections")

            onActiveFocusChanged: if (!activeFocus) root._cursor = -1

            function move(delta) {
                const last = root._rows.length - 1;
                root._cursor = root._cursor < 0 ? 0 : Math.max(0, Math.min(last, root._cursor + delta));
                root.activate(root._cursor);
            }
            Keys.onDownPressed: move(1)
            Keys.onUpPressed: move(-1)
            Keys.onReturnPressed: if (root._cursor >= 0) root.activate(root._cursor)
            Keys.onSpacePressed: if (root._cursor >= 0) root.activate(root._cursor)

            Column {
                id: rowsColumn
                width: sidebarList.width

                Repeater {
                    model: root._rows

                    delegate: Rectangle {
                        id: row
                        required property int index
                        required property var modelData
                        readonly property bool isSelected: root.isSelected(row.modelData)
                        readonly property string title: row.modelData.kind === "profile" ? row.modelData.name : row.modelData.title
                        objectName: row.modelData.kind === "section" ? "sidebarRow:" + row.modelData.id
                                    : row.modelData.kind === "profile" ? "profileRow:" + row.modelData.name
                                    : "editConfigRow"
                        width: rowsColumn.width
                        height: chromeStyle.settings.sidebarRow
                        radius: chromeStyle.radii.control
                        color: row.isSelected ? chromeStyle.colors.settingsSelectionFill : "transparent"

                        Accessible.role: Accessible.ListItem
                        Accessible.name: row.title
                        Accessible.selectable: true
                        Accessible.selected: row.isSelected
                        Accessible.onPressAction: root.activate(row.index)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: chromeStyle.space.s4
                            anchors.rightMargin: chromeStyle.space.s4
                            spacing: chromeStyle.space.s4

                            // The icon: a tile behind the glyph where the style draws one, the glyph alone
                            // where not.
                            Rectangle {
                                objectName: "sidebarIconTile"
                                visible: row.modelData.kind === "section"
                                Layout.preferredWidth: Math.max(chromeStyle.settings.iconTile, chromeStyle.settings.iconGlyph)
                                Layout.preferredHeight: Layout.preferredWidth
                                radius: chromeStyle.radii.control
                                color: chromeStyle.settings.iconTile > 0 ? chromeStyle.colors.settingsIconTile : "transparent"

                                Label {
                                    anchors.centerIn: parent
                                    text: row.modelData.kind === "section" ? root.iconGlyphs[row.modelData.icon] : ""
                                    font: chromeStyle.type.body
                                    color: row.isSelected ? chromeStyle.colors.settingsSelectionText
                                                          : chromeStyle.colors.settingsIconGlyph
                                    Accessible.ignored: true
                                }
                            }

                            // A profile's tab colour, where it has one.
                            Rectangle {
                                objectName: "profileTabColor"
                                visible: row.modelData.kind === "profile" && row.modelData.tabColor.length > 0
                                Layout.preferredWidth: chromeStyle.space.s5
                                Layout.preferredHeight: Layout.preferredWidth
                                radius: width * root._pill
                                color: visible ? row.modelData.tabColor : "transparent"
                            }

                            Label {
                                Layout.fillWidth: true
                                text: row.title
                                font: chromeStyle.type.body
                                color: row.isSelected ? chromeStyle.colors.settingsSelectionText
                                                      : chromeStyle.colors.settingsLabel
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                Accessible.ignored: true
                            }

                            Label {
                                visible: row.modelData.kind === "profile" && row.modelData.isDefault === true
                                text: qsTr("default")
                                font: chromeStyle.type.footnote
                                color: row.isSelected ? chromeStyle.colors.settingsSelectionText
                                                      : chromeStyle.colors.settingsHelp
                                Accessible.ignored: true
                            }
                        }

                        // The keyboard's place.
                        Rectangle {
                            objectName: "sidebarCursor"
                            anchors.fill: parent
                            radius: parent.radius
                            color: "transparent"
                            border.width: chromeStyle.borderWidth
                            border.color: chromeStyle.colors.settingsLabel
                            visible: sidebarList.activeFocus && root._cursor === row.index
                        }

                        TapHandler {
                            onTapped: {
                                root._cursor = row.index;
                                root.activate(row.index);
                            }
                        }
                    }
                }
            }
        }

        // "+ − …": add a profile, remove the selected one, and the rest of what a profile offers. The three share
        // the sidebar's width, so their implicit widths never push the sidebar's contents past its edge.
        RowLayout {
            Layout.fillWidth: true
            spacing: chromeStyle.space.s3

            Button {
                objectName: "addProfileButton"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                text: "+"
                enabled: root.editable
                Accessible.name: qsTr("Add a profile")
                onClicked: root.addProfileRequested()
            }
            Button {
                objectName: "removeProfileButton"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                text: "−"
                enabled: root.editable && root.selectedProfile.length > 0
                Accessible.name: qsTr("Remove the selected profile")
                onClicked: root.removeProfileRequested()
            }
            Button {
                objectName: "profileMenuButton"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                text: "…"
                enabled: root.editable && root.selectedProfile.length > 0
                Accessible.name: qsTr("More about the selected profile")
                onClicked: profileMenu.popup(parent, 0, parent.height)
            }
        }
    }

    ContourMenu {
        id: profileMenu
        objectName: "profileMenu"
        MenuItem {
            objectName: "profileMenuDuplicate"
            text: qsTr("Duplicate")
            onTriggered: root.profileActionRequested("duplicate")
        }
        MenuItem {
            objectName: "profileMenuRename"
            text: qsTr("Rename…")
            onTriggered: root.profileActionRequested("rename")
        }
        MenuItem {
            objectName: "profileMenuDefault"
            text: qsTr("Set as Default")
            onTriggered: root.profileActionRequested("default")
        }
    }

    // A tab colour's dot is a circle: its radius is this share of its width.
    readonly property real _pill: 0.5
}
