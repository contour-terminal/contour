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
    // Height is the background's alone, NOT the usual max() against the content. The background is
    // chromeStyle.controlHeight -- a whole number of character cells, which is the one thing this
    // style guarantees. A cell is ceil(lineSpacing()), so on a font whose line spacing is not an
    // integral number of pixels the text's own implicit height is the larger of the two, and a max()
    // would let it push the control a fraction of a cell taller and off the grid. The content is
    // clipped to the cell instead, which is what a terminal does with an oversized glyph anyway.
    implicitHeight: implicitBackgroundHeight + topInset + bottomInset

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
        // Pinned to the cell grid rather than left at the Text's own natural line height. A cell is
        // ceil(lineSpacing()), so on a font whose line spacing is not a whole number of pixels the
        // raw content is the taller of the two -- and the control's implicitHeight is a Math.max, so
        // the content would win and make the button a fraction of a cell taller than a cell. That is
        // exactly the invariant this style exists to hold, so the text is told the cell height.
        height: chromeStyle.cellHeight
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
