// vim:syntax=qml
// CheckBox for the ContourTui style, drawn the way a TUI draws one: a bracketed cell, "[x]" when
// checked, "[-]" when partially checked, "[ ]" otherwise. Text rather than a painted box, so it scales
// with the font and sits on the grid like everything else. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.CheckBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: 0
    spacing: chromeStyle.labelGap
    font: chromeStyle.font

    indicator: Text {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + ((control.availableHeight - height) / 2)
        text: control.checkState === Qt.Checked ? "[x]"
            : control.checkState === Qt.PartiallyChecked ? "[-]"
            : "[ ]"
        font: control.font
        color: control.palette.text
        opacity: control.enabled ? 1.0 : 0.5
        verticalAlignment: Text.AlignVCenter
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.palette.windowText
        opacity: control.enabled ? 1.0 : 0.5
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
