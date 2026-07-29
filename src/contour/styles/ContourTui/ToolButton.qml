// vim:syntax=qml
// ToolButton for the ContourTui Qt Quick Controls style.
//
// Every file in this directory is a Qt Quick Controls style implementation: a QtQuick.Templates
// control (the behaviour, with no appearance of its own) given a contentItem and a background. The
// style is selected app-wide by QQuickStyle::setStyle("ContourTui") in ContourGuiApp when
// `ui_style: terminal` is configured, which is why these files never test for the style themselves.
//
// The metrics come from the `chromeStyle` context property (UiStyleProvider), so a control is a whole
// number of character cells and shares the terminal's font. Colors keep coming from the control's own
// palette, so the OS light/dark theme and the `theme` setting still drive them.
import QtQuick
import QtQuick.Templates as T

T.ToolButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    // No padding at all, unlike Button.qml. A ToolButton in this chrome is a glyph box whose width
    // the caller pins from the token table -- one cell for a tab's close affordance, wider for the
    // strip's "+" and "▾". At one cell, a cell of padding on each side would leave the glyph negative
    // width to lay out in and it simply would not draw; the padding such a button wants belongs in
    // its width token, where the style can state it.
    padding: 0
    spacing: chromeStyle.labelGap

    font: chromeStyle.font

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.palette.buttonText
        opacity: control.enabled ? 1.0 : 0.5
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: chromeStyle.controlWidth
        implicitHeight: chromeStyle.controlHeight
        radius: chromeStyle.radius
        // Pressed and checked read as "selected", which in a terminal is inverse video; hover is the
        // same faint highlight wash the hand-drawn chrome uses, so the two agree.
        color: control.down || control.checked
               ? control.palette.highlight
               : control.hovered ? chromeStyle.wash(control.palette.highlight)
                                 : "transparent"
    }
}
