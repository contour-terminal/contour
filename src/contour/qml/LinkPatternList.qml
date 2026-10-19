// vim:syntax=qml
// The link pattern table of the profile being edited: the built-ins first, whose Clickable switch is all that
// can change, then the profile's own patterns, with a live preview of what the selected one finds in a sample.
//
// The controller is the single source of truth: every write goes through commit(), which hands the whole row
// to SettingsController.setLinkPattern, and every row, its problem and the preview are read back from it. The
// controller also checks each pattern, as the terminal will, and refuses a read-only profile; `editable` here
// is a courtesy on top of that boundary.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "linkPatternList"

    property var controller: null
    property bool editable: false
    /// The row the preview shows: the last one whose name or regex field had the focus.
    property int selected: 0
    property string sample: "see https://example.com/a_(b), src/main.cpp:12:5 and ABC-123."

    /// Asks the page to confirm removing row @p index, the profile's pattern @p name.
    signal deleteRequested(int index, string name)

    spacing: 6

    // Live OS palette handle, so the list follows dark/light in realtime (see CommandPalette.qml).
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }
    readonly property color subtleText: Qt.rgba(sys.windowText.r, sys.windowText.g, sys.windowText.b, 0.6)

    readonly property var rows: root.controller ? root.controller.linkPatterns : []

    /// Writes @p value as row @p index's @p key: the list's one write path.
    function commit(index, key, value) {
        if (!root.controller || !root.editable || index < 0 || index >= root.rows.length)
            return
        var edited = Object.assign({}, root.rows[index])
        edited[key] = value
        root.controller.setLinkPattern(index, edited)
    }

    /// What the selected row finds in the sample, as the controller reports it.
    readonly property var preview: (root.controller && root.selected < root.rows.length)
                                   ? root.controller.previewLinkPattern(root.rows[root.selected], root.sample)
                                   : ({ spans: [], problem: "", note: "" })

    /// The sample with each span the preview found underlined, as rich text.
    function previewHtml() {
        var spans = root.preview.spans || []
        var escape = function(text) {
            return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        }
        var out = ""
        var at = 0
        for (var i = 0; i < spans.length; ++i) {
            if (spans[i].start < at)
                continue // an overlapping match: hint mode keeps both, the preview underlines the first
            out += escape(root.sample.substring(at, spans[i].start))
            // A match that is no link (the printed text names no scheme Contour opens) is struck out.
            var tag = spans[i].refused ? "s" : "u"
            out += "<" + tag + ">" + escape(root.sample.substring(spans[i].start, spans[i].end)) + "</" + tag + ">"
            at = spans[i].end
        }
        return out + escape(root.sample.substring(at))
    }

    // One delegate per row, made again only when the count changes: every commit changes the model, and a
    // delegate made anew for it would take the focus out of the field the user just moved to. Each row reads
    // its values from the model by index, so a commit updates the fields in place.
    Repeater {
        model: root.rows.length
        delegate: ColumnLayout {
            id: patternRow
            required property int index
            /// This row as the controller has it; empty while the model shrinks under the delegate.
            readonly property var row: root.rows[index] || ({ name: "", regex: "", clickable: false, url: "",
                                                               builtin: true, kind: "", problem: "" })
            Layout.fillWidth: true
            spacing: 2

            /// Commits @p field's text as this row's @p key, then shows the stored value again: a refused
            /// edit leaves the model as it was, and only this takes the typed text back out of the field.
            function commitText(field, key) {
                if (field.text !== patternRow.row[key])
                    root.commit(patternRow.index, key, field.text)
                field.text = Qt.binding(() => patternRow.row[key])
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                TextField {
                    id: nameField
                    objectName: "linkPatternName"
                    text: patternRow.row.name
                    readOnly: patternRow.row.builtin || !root.editable
                    Layout.preferredWidth: 110
                    onEditingFinished: patternRow.commitText(nameField, "name")
                    onActiveFocusChanged: if (activeFocus) root.selected = patternRow.index
                }
                TextField {
                    id: regexField
                    objectName: "linkPatternRegex"
                    text: patternRow.row.regex
                    readOnly: patternRow.row.builtin || !root.editable
                    placeholderText: qsTr("Regular expression")
                    Layout.fillWidth: true
                    font.family: "monospace"
                    onEditingFinished: patternRow.commitText(regexField, "regex")
                    onActiveFocusChanged: if (activeFocus) root.selected = patternRow.index
                }
                Switch {
                    objectName: "linkPatternClickable"
                    checked: patternRow.row.clickable
                    enabled: root.editable
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Detect in output, decorate, and open with the follow gesture")
                    onToggled: {
                        root.commit(patternRow.index, "clickable", checked)
                        checked = Qt.binding(() => patternRow.row.clickable)
                    }
                }
                TextField {
                    id: urlField
                    objectName: "linkPatternUrl"
                    text: patternRow.row.url
                    visible: !patternRow.row.builtin
                    readOnly: !root.editable
                    placeholderText: qsTr("URL template, e.g. https://example.com/{0}")
                    Layout.preferredWidth: 220
                    onEditingFinished: patternRow.commitText(urlField, "url")
                    onActiveFocusChanged: if (activeFocus) root.selected = patternRow.index
                }
                ToolButton {
                    objectName: "deleteLinkPatternButton"
                    visible: !patternRow.row.builtin && root.editable
                    text: "🗑"
                    Accessible.name: qsTr("Delete")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Remove this pattern")
                    onClicked: root.deleteRequested(patternRow.index, patternRow.row.name)
                }
            }
            Label {
                objectName: "linkPatternProblem"
                visible: text.length > 0
                text: patternRow.row.problem
                color: chromeStyle.errorColor
                font.pointSize: 9
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }

    Button {
        objectName: "addLinkPatternButton"
        text: qsTr("Add pattern")
        enabled: root.editable
        onClicked: if (root.controller) root.controller.addLinkPattern()
    }

    Label {
        text: qsTr("Preview: what the selected pattern finds in this text")
        color: root.subtleText
        Layout.topMargin: 6
    }
    TextField {
        objectName: "linkPatternSample"
        Layout.fillWidth: true
        text: root.sample
        onTextChanged: root.sample = text
    }
    Label {
        objectName: "linkPatternPreview"
        Layout.fillWidth: true
        textFormat: Text.RichText
        wrapMode: Text.WordWrap
        text: root.previewHtml()
    }
    Label {
        objectName: "linkPatternNote"
        visible: text.length > 0
        Layout.fillWidth: true
        text: root.preview.note || ""
        color: root.subtleText
        font.pointSize: 9
        wrapMode: Text.WordWrap
    }
}
