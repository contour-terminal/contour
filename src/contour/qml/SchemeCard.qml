// vim:syntax=qml
// One colour scheme in the gallery: how it looks (its background with its foreground on it, and its eight
// normal colours as a strip) and its name, with a check where the profile draws with it. Opening it, by the
// pointer or Return and Space, asks for it to be edited.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Control {
    id: root

    /// The gallery's card of the scheme: { name, editable, background, foreground, normal: [8 x "#rrggbb"] }.
    property var card: ({ name: "", editable: false, background: "transparent", foreground: "transparent", normal: [] })
    /// Whether the scheme is the one open for editing.
    property bool isEditing: false
    /// Whether the profile draws with the scheme.
    property bool isChosen: false

    /// The card was asked to open its scheme.
    signal opened()

    // What the padding takes of its width and height, at both edges.
    readonly property real _padding: 2 * chromeStyle.settings.rowPadding

    objectName: "schemeCard:" + root.card.name
    // As wide as its strip of colour wells, plus the padding the rows have.
    implicitWidth: root.card.normal.length * chromeStyle.settings.colorWell + root._padding
    implicitHeight: column.implicitHeight + root._padding
    padding: chromeStyle.settings.rowPadding
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: root.isChosen ? qsTr("%1, in use").arg(root.card.name) : root.card.name
    Accessible.onPressAction: root.opened()
    Keys.onReturnPressed: root.opened()
    Keys.onSpacePressed: root.opened()

    // The edge: the full ink for the scheme open for editing, the quiet separator for the others, and a ring
    // outside it while the card has the keyboard.
    background: Rectangle {
        objectName: "schemeCardFrame"
        radius: chromeStyle.settings.groupRadius
        color: chromeStyle.colors.settingsGroupFill
        border.width: chromeStyle.borderWidth
        border.color: root.isEditing ? chromeStyle.colors.settingsLabel : chromeStyle.colors.settingsSeparator

        Rectangle {
            objectName: "schemeCardFocusRing"
            anchors.fill: parent
            anchors.margins: -chromeStyle.borderWidth
            radius: parent.radius
            color: "transparent"
            border.width: chromeStyle.borderWidth
            border.color: chromeStyle.colors.settingsSelectionFill
            visible: root.visualFocus
        }
    }

    contentItem: Column {
        id: column
        spacing: chromeStyle.space.s3

        // The scheme's own look: its foreground on its background.
        Rectangle {
            width: parent.width
            height: chromeStyle.settings.control
            radius: chromeStyle.radii.control
            color: root.card.background

            Label {
                objectName: "schemeCardSample"
                anchors.verticalCenter: parent.verticalCenter
                x: chromeStyle.space.s3
                text: "Aa"
                font: chromeStyle.type.body
                color: root.card.foreground
                Accessible.ignored: true
            }
        }

        // The eight normal colours.
        Row {
            Repeater {
                model: root.card.normal

                delegate: Rectangle {
                    required property string modelData
                    objectName: "schemeCardSwatch"
                    width: chromeStyle.settings.colorWell
                    height: chromeStyle.settings.control
                    color: modelData
                }
            }
        }

        Row {
            spacing: chromeStyle.space.s3

            Label {
                id: checkMark
                objectName: "schemeCardCheck"
                visible: root.isChosen
                text: "✓"
                font: chromeStyle.type.body
                color: chromeStyle.colors.settingsLabel
                Accessible.ignored: true
            }
            Label {
                objectName: "schemeCardName"
                width: column.width - (checkMark.visible ? checkMark.width + parent.spacing : 0)
                text: root.card.name
                font: chromeStyle.type.body
                color: chromeStyle.colors.settingsLabel
                elide: Text.ElideRight
                textFormat: Text.PlainText
                Accessible.ignored: true
            }
        }
    }

    TapHandler {
        onTapped: root.opened()
    }
}
