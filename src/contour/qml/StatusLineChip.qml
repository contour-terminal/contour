// vim:syntax=qml
// One placeholder of the indicator status line, shown as a chip: its name, a compact badge of the styles
// it carries, and a remove button. Clicking the chip body asks to edit it.
//
// It takes the item map SettingsController::parseIndicatorSegment produced and reports intent through
// signals; it neither reads the controller nor writes to it. Its own file rather than an inline component
// inside StatusLineIndicatorEditor.qml, because an inline component is not usable as a Repeater delegate
// here -- it silently produces nothing, so the segments rendered empty -- and because a component with
// declared inputs and signals is the shape everything else in this directory already has.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    /// One entry of SettingsController::parseIndicatorSegment.
    required property var item
    /// The template flag names, in display order; used to build the style badge.
    property var flagKeys: []
    property bool editable: true

    signal editRequested()
    signal removeRequested()

    /// Which attributes this item carries, without their values: one letter per set flag, plus a mark for
    /// each colour. Enough to tell two otherwise identical chips apart at a glance.
    readonly property string badge: {
        var parts = []
        var flags = root.item && root.item.flags !== undefined ? root.item.flags : ({})
        for (var i = 0; i < root.flagKeys.length; ++i)
            if (flags[root.flagKeys[i]] === true)
                parts.push(String(root.flagKeys[i]).charAt(0))
        if (root.item && root.item.hasColor === true)
            parts.push("◉")
        if (root.item && root.item.hasBgColor === true)
            parts.push("▣")
        return parts.join("")
    }

    readonly property string displayName: root.item && root.item.displayName !== undefined
                                         ? root.item.displayName : ""

    implicitHeight: chipRow.implicitHeight + chromeStyle.space.s2 + chromeStyle.space.s2
    implicitWidth: chipRow.implicitWidth + chromeStyle.space.s4 + chromeStyle.space.s4
    radius: chromeStyle.radii.control
    color: chromeStyle.colors.controlFill
    // The hovered chip's edge is the label's ink, a mark that stands off the fill.
    border.width: chromeStyle.borderWidth
    border.color: chipHover.hovered ? chromeStyle.colors.settingsLabel : chromeStyle.colors.settingsSeparator
    opacity: root.editable ? 1.0 : chromeStyle.settings.disabledOpacity

    HoverHandler { id: chipHover; enabled: root.editable }

    // Declared BEFORE the row, so the remove button inside the row stacks above it. As a later sibling
    // this filled the chip and swallowed every click meant for the remove button, which made removing a
    // placeholder by its own button impossible.
    MouseArea {
        anchors.fill: parent
        enabled: root.editable
        cursorShape: root.editable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.editRequested()
    }

    RowLayout {
        id: chipRow
        anchors.centerIn: parent
        spacing: chromeStyle.space.s3

        Label {
            text: root.displayName
            color: chromeStyle.colors.controlText
            font: chromeStyle.type.footnote
            elide: Text.ElideRight
            Layout.maximumWidth: chromeStyle.settings.sidebarWidth
        }

        Label {
            visible: root.badge.length > 0
            text: root.badge
            color: chromeStyle.colors.controlText
            font: Qt.font({ family: "monospace", pixelSize: chromeStyle.type.footnote.pixelSize })
        }

        // A real button, so it is keyboard reachable and owns its own hit area rather than being a
        // MouseArea grafted onto a Label.
        ToolButton {
            objectName: "indicatorChipRemoveButton"
            text: "✕"
            flat: true
            visible: root.editable
            font: chromeStyle.type.footnote
            Accessible.name: qsTr("Remove %1").arg(root.displayName)
            onClicked: root.removeRequested()
        }
    }
}
