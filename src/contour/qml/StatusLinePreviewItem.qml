// vim:syntax=qml
// One placeholder as it will appear in the rendered indicator status line: its stand-in text with the
// styling the item carries actually applied.
//
// A preview, not the real thing — the true value of {Clock} or {InputMode} needs a live terminal, which a
// settings page does not have, so each placeholder shows the sample from
// vtbackend::StatusLineDefinitions::ItemTraits instead. What *is* real is the styling: the flags, the
// colours and the side adornments are the ones that will be written to the profile.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    /// One entry of SettingsController::parseIndicatorSegment.
    required property var item

    // Live OS palette handle, so the preview follows dark/light in realtime.
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    function flagOf(key) {
        var flags = root.item && root.item.flags !== undefined ? root.item.flags : ({})
        return flags[key] === true
    }

    readonly property string previewText: {
        if (!root.item)
            return ""
        var left = root.item.textLeft !== undefined ? root.item.textLeft : ""
        var right = root.item.textRight !== undefined ? root.item.textRight : ""
        // Text renders its own content; every other placeholder stands in with its sample.
        var middle = root.item.type === "Text" ? (root.item.text !== undefined ? root.item.text : "")
                                               : (root.item.sample !== undefined ? root.item.sample : "")
        return left + middle + right
    }

    implicitWidth: label.implicitWidth
    implicitHeight: label.implicitHeight
    color: root.item && root.item.hasBgColor === true ? root.item.bgColor : "transparent"

    Label {
        id: label
        text: root.previewText
        color: root.item && root.item.hasColor === true ? root.item.color : sys.windowText
        font.pointSize: 9
        font.bold: root.flagOf("Bold")
        font.italic: root.flagOf("Italic")
        font.underline: root.flagOf("Underline")
        font.strikeout: root.flagOf("CrossedOut")
        // Faint has no font property of its own; the palette carries it, as it does in the terminal.
        opacity: root.flagOf("Faint") ? 0.55 : 1.0
    }
}
