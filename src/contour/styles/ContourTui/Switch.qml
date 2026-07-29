// vim:syntax=qml
// Switch for the ContourTui style. A sliding thumb has no terminal equivalent, so this is the TUI
// spelling of the same thing: a bracketed state word, "[on ]" / "[off]", padded to a constant width so
// a column of settings rows stays aligned as their values change.
//
// This is the control the settings page uses for every boolean, so it is what most of that page looks
// like under this style. See ToolButton.qml for what a style file is.
import QtQuick
import QtQuick.Templates as T

T.Switch {
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
        text: control.checked ? "[on ]" : "[off]"
        font: control.font
        // Checked is the state that matters, so it takes the highlight; unchecked stays plain text
        // rather than dimmed, because a settings row must still be readable when it is off.
        color: control.checked ? control.palette.highlight : control.palette.text
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
