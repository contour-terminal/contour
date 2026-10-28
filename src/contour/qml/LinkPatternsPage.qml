// vim:syntax=qml
// A profile's Links page: the gesture that opens links, the table of the patterns that find them (the built-ins
// first, whose Clickable switch is all that can change, then the profile's own), the detail of the selected
// pattern of the profile's own, and a sample text with what the selected pattern finds in it.
//
// The controller is the single source of truth: every write goes through commit(), which hands the whole row to
// SettingsController.setLinkPattern, and every row, its problem and the preview are read back from it. The
// controller also checks each pattern, as the terminal will, holds one in error back from the file, and
// refuses a read-only profile; `editable` here is a courtesy on top of that boundary.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

SettingsGroupsPage {
    id: root
    objectName: "linkPatternsPage"

    /// The SettingsController.
    property var controller: null
    /// Whether the profile can be edited.
    property bool profileEditable: root.controller !== null && !root.controller.locked
                                   && !root.controller.editingReadOnly
    /// The row the detail and the preview show.
    property int selected: 0
    /// The text the preview looks for links in.
    property string sample: "see https://example.com/a_(b), src/main.cpp:12:5 and ABC-123."

    /// Asks the page that owns the confirmation to remove row @p index, the profile's pattern @p name.
    signal deleteRequested(int index, string name)

    readonly property var rows: root.controller ? root.controller.linkPatterns : []
    // The row shown, as the controller has it; empty while the model shrinks under the page.
    readonly property var _selectedRow: root.rows[root.selected] !== undefined ? root.rows[root.selected] : null
    readonly property bool _selectedIsOwn: root._selectedRow !== null && !root._selectedRow.builtin
    // How a pattern's kind reads in the table.
    readonly property var _kindTitles: ({ url: qsTr("URL"), path: qsTr("Path"), custom: qsTr("Custom") })

    /// What the selected row finds in the sample, as the controller reports it.
    readonly property var preview: root._selectedRow !== null
                                   ? root.controller.previewLinkPattern(root._selectedRow, root.sample)
                                   : ({ spans: [], problem: "", note: "" })

    /// Writes @p value as row @p index's @p key: the page's one write path.
    function commit(index, key, value) {
        if (!root.controller || !root.profileEditable || index < 0 || index >= root.rows.length)
            return;
        const edited = Object.assign({}, root.rows[index]);
        edited[key] = value;
        root.controller.setLinkPattern(index, edited);
    }

    /// The sample with each span the preview found underlined, as rich text.
    function previewHtml() {
        const spans = root.preview.spans || [];
        const escape = text => text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
        let out = "";
        let at = 0;
        for (const span of spans) {
            if (span.start < at)
                continue; // an overlapping match: hint mode keeps both, the preview underlines the first
            out += escape(root.sample.substring(at, span.start));
            // A match that is no link (the printed text names no scheme Contour opens) is struck out.
            const tag = span.refused ? "s" : "u";
            out += "<" + tag + ">" + escape(root.sample.substring(span.start, span.end)) + "</" + tag + ">";
            at = span.end;
        }
        return out + escape(root.sample.substring(at));
    }

    /// Adds a pattern of the profile's own and selects it.
    function addPattern() {
        root.controller.addLinkPattern();
        root.selected = root.controller.linkPatterns.length - 1;
    }

    /// Keeps the selection on a row the table has.
    onRowsChanged: if (root.selected >= root.rows.length) root.selected = Math.max(0, root.rows.length - 1)

    /// @return The field row of the detail's @p key (name, regex or url) for a SettingsRow.
    function detailField(key, label, controlObjectName) {
        return { key: "link." + key, label: label, help: "", controlName: "textField", controlObjectName: controlObjectName,
                 value: root._selectedIsOwn && root._selectedRow[key] !== undefined ? root._selectedRow[key] : "" };
    }

    header: [
        // How links are opened.
        Label {
            objectName: "linkGestureLabel"
            Layout.fillWidth: true
            Layout.leftMargin: chromeStyle.settings.rowPadding
            wrapMode: Text.Wrap
            text: root.controller ? root.controller.linkGesture : ""
            font: chromeStyle.type.body
            color: chromeStyle.colors.settingsLabel
            textFormat: Text.PlainText
        },

        // The patterns that find links.
        SettingsGroup {
            objectName: "linkPatternsTable"
            Layout.fillWidth: true
            title: qsTr("Link patterns")

            // One delegate per row, made again only when the count changes: every commit changes the model,
            // and a delegate made anew for it would take the focus out of the control the user just moved to.
            // Each row reads its values from the model by index, so a commit updates them in place.
            Repeater {
                model: root.rows.length

                delegate: Control {
                    id: patternRow
                    required property int index
                    /// This row as the controller has it; empty while the model shrinks under the delegate.
                    readonly property var row: root.rows[index] !== undefined
                                               ? root.rows[index]
                                               : ({ name: "", clickable: false, builtin: true, kind: "", problem: "" })
                    readonly property bool isSelected: index === root.selected

                    objectName: "linkPatternRow:" + index
                    Layout.fillWidth: true
                    implicitHeight: rowColumn.implicitHeight + chromeStyle.space.s3 + chromeStyle.space.s3
                    activeFocusOnTab: true

                    Accessible.role: Accessible.ListItem
                    Accessible.name: patternRow.row.name
                    Accessible.selectable: true
                    Accessible.selected: patternRow.isSelected
                    Accessible.onPressAction: root.selected = patternRow.index
                    Keys.onReturnPressed: root.selected = patternRow.index
                    Keys.onSpacePressed: root.selected = patternRow.index

                    // The ring of the row the detail shows.
                    background: Rectangle {
                        objectName: "linkPatternRowFrame"
                        anchors.fill: parent
                        anchors.margins: chromeStyle.borderWidth
                        radius: chromeStyle.radii.control
                        color: "transparent"
                        border.width: chromeStyle.borderWidth
                        border.color: patternRow.isSelected ? chromeStyle.colors.settingsLabel
                                      : patternRow.visualFocus ? chromeStyle.colors.settingsSelectionFill
                                                               : "transparent"
                    }

                    contentItem: ColumnLayout {
                        id: rowColumn
                        spacing: chromeStyle.space.s1

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: chromeStyle.settings.rowPadding
                            Layout.rightMargin: chromeStyle.settings.rowPadding
                            spacing: chromeStyle.space.s6

                            Label {
                                objectName: "linkPatternRowName"
                                Layout.fillWidth: true
                                text: patternRow.row.name
                                font: chromeStyle.type.body
                                color: chromeStyle.colors.settingsLabel
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                Accessible.ignored: true
                            }
                            Label {
                                objectName: "linkPatternKind"
                                text: root._kindTitles[patternRow.row.kind] !== undefined
                                      ? root._kindTitles[patternRow.row.kind] : patternRow.row.kind
                                font: chromeStyle.type.footnote
                                color: chromeStyle.colors.settingsHelp
                                Accessible.ignored: true
                            }
                            KitSwitch {
                                objectName: "linkPatternClickable:" + patternRow.index
                                checked: patternRow.row.clickable === true
                                name: qsTr("Clickable")
                                description: qsTr("Detect in output, decorate, and open with the follow gesture")
                                editable: root.profileEditable
                                onToggled: checked => {
                                    root.selected = patternRow.index;
                                    root.commit(patternRow.index, "clickable", checked);
                                }
                            }
                        }

                        // What is wrong with the pattern, under its row.
                        Label {
                            objectName: "linkPatternProblem"
                            Layout.fillWidth: true
                            Layout.leftMargin: chromeStyle.settings.rowPadding
                            Layout.rightMargin: chromeStyle.settings.rowPadding
                            visible: text.length > 0
                            text: patternRow.row.problem
                            font: chromeStyle.type.footnote
                            color: chromeStyle.statusInk(chromeStyle.statusDanger)
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                    }

                    TapHandler {
                        onTapped: root.selected = patternRow.index
                    }
                }
            }

            // "+" adds a pattern of the profile's own; "−" removes the selected one, if it is.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: chromeStyle.settings.rowPadding
                Layout.rightMargin: chromeStyle.settings.rowPadding
                spacing: chromeStyle.space.s3

                Button {
                    objectName: "addLinkPatternButton"
                    enabled: root.profileEditable
                    text: "+"
                    font: chromeStyle.type.body
                    Accessible.name: qsTr("Add link pattern")
                    onClicked: root.addPattern()
                }
                Button {
                    objectName: "removeLinkPatternButton"
                    enabled: root.profileEditable && root._selectedIsOwn
                    text: "−"
                    font: chromeStyle.type.body
                    Accessible.name: qsTr("Remove link pattern")
                    onClicked: root.deleteRequested(root.selected, root._selectedRow.name)
                }
                Item { Layout.fillWidth: true }
            }
        },

        // The selected pattern of the profile's own.
        SettingsGroup {
            objectName: "linkPatternDetail"
            Layout.fillWidth: true
            visible: root._selectedIsOwn
            title: qsTr("Pattern")

            SettingsRow {
                Layout.fillWidth: true
                showSeparator: true
                editable: root.profileEditable
                field: root.detailField("name", qsTr("Name"), "linkPatternName")
                onCommitted: (key, value) => root.commit(root.selected, "name", value)
            }
            SettingsRow {
                Layout.fillWidth: true
                showSeparator: true
                editable: root.profileEditable
                field: root.detailField("regex", qsTr("Regular expression"), "linkPatternRegex")
                onCommitted: (key, value) => root.commit(root.selected, "regex", value)
            }
            SettingsRow {
                Layout.fillWidth: true
                editable: root.profileEditable
                field: root.detailField("url", qsTr("URL template"), "linkPatternUrl")
                onCommitted: (key, value) => root.commit(root.selected, "url", value)
            }
        },

        // What the selected pattern finds in a text of the user's.
        SettingsGroup {
            objectName: "linkSampleGroup"
            Layout.fillWidth: true
            title: qsTr("Preview")

            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: chromeStyle.settings.rowPadding
                Layout.rightMargin: chromeStyle.settings.rowPadding
                implicitHeight: chromeStyle.settings.control
                radius: chromeStyle.radii.control
                color: chromeStyle.colors.controlFill

                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Sample text")

                TextField {
                    objectName: "linkSampleField"
                    anchors.fill: parent
                    background: null
                    font: chromeStyle.type.body
                    color: chromeStyle.colors.controlText
                    selectByMouse: true
                    text: root.sample
                    onTextChanged: root.sample = text
                    Accessible.ignored: true
                }
            }
            Label {
                objectName: "linkSamplePreview"
                Layout.fillWidth: true
                Layout.leftMargin: chromeStyle.settings.rowPadding
                Layout.rightMargin: chromeStyle.settings.rowPadding
                textFormat: Text.RichText
                wrapMode: Text.WordWrap
                text: root.previewHtml()
                font: chromeStyle.type.body
                color: chromeStyle.colors.settingsLabel
            }
            Label {
                objectName: "linkPatternNote"
                Layout.fillWidth: true
                Layout.leftMargin: chromeStyle.settings.rowPadding
                Layout.rightMargin: chromeStyle.settings.rowPadding
                visible: text.length > 0
                text: root.preview.note || ""
                font: chromeStyle.type.footnote
                color: chromeStyle.colors.settingsHelp
                wrapMode: Text.WordWrap
            }
        }
    ]
}
