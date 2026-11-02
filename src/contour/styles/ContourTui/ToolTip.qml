// vim:syntax=qml
// ToolTip for the ContourTui style: any attached tooltip (the attached one is the stray case; the kit's own
// controls use qml/KitToolTip.qml) drawn from the same tooltip tokens, placed as KitToolTip is. Opaque for the
// reason Popup.qml states. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T
import Contour.Ui

T.ToolTip {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    leftPadding: chromeStyle.size.tooltipPadX
    rightPadding: chromeStyle.size.tooltipPadX
    topPadding: chromeStyle.size.tooltipPadTop
    bottomPadding: chromeStyle.size.tooltipPadBottom

    // The window keeps the box, and its shadow, this far from its edges.
    margins: background.shadowReach

    // Placed as KitToolTip is: below, above where there is no room, once as it begins to show.
    KitToolTipPlacer {
        id: placer
        popup: control
    }
    onAboutToShow: placer.place()

    contentItem: Text {
        text: control.text
        // Never AutoText: a tooltip's text may be what an application wrote (a link's URI, a tab title, a
        // command line), and AutoText renders it as rich text whenever its first line looks like markup.
        textFormat: Text.PlainText
        font: chromeStyle.type.footnote
        color: chromeStyle.colors.tooltipText
        wrapMode: Text.WordWrap
    }

    background: KitToolTipBackground {}
}
